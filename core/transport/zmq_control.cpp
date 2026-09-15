#include "zmq_control.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <zmq.hpp>

namespace nexweave::transport {
namespace {

using domain::ErrorCode;
using domain::OperationResult;
using protocol::ControlRequest;
using protocol::ControlResponse;

template <typename T>
domain::Result<T> failure(ErrorCode code, std::string message = {}) {
  return domain::Result<T>::failure(code, std::move(message));
}

std::string first_line(const std::string& bytes) {
  const std::size_t newline = bytes.find('\n');
  if (newline == std::string::npos) {
    return bytes;
  }
  return bytes.substr(0, newline);
}

int socket_timeout_millis(std::chrono::milliseconds value) {
  if (value.count() > std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  if (value.count() < 1) {
    return 1;
  }
  return static_cast<int>(value.count());
}

std::string make_transport_error_response(const std::string& payload, ErrorCode code,
                                          const std::string& message) {
  const auto request = protocol::decode_request(payload);
  if (!request.ok()) {
    return {};
  }
  ControlResponse response;
  response.request_id = request.value->request_id;
  response.result = OperationResult::failure(code, message);
  const auto encoded = protocol::encode_response(response);
  return encoded.ok() ? *encoded.value : std::string();
}

}  // namespace

domain::OperationResult validate_zmq_control_server_config(
    const ZmqControlServerConfig& config) {
  if (config.endpoint.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 端点不能为空");
  }
  if (config.receive_high_water_mark == 0 || config.send_high_water_mark == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 水位必须为正");
  }
  if (config.poll_interval.count() <= 0 || config.send_timeout.count() <= 0 ||
      config.bind_wait_budget.count() <= 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 等待预算必须为正");
  }
  if (config.max_response_bytes == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "响应字节预算必须为正");
  }
  if (config.receive_high_water_mark > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      config.send_high_water_mark > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 水位超出 int 范围");
  }
  if (config.poll_interval.count() > std::numeric_limits<int>::max() ||
      config.send_timeout.count() > std::numeric_limits<int>::max() ||
      config.bind_wait_budget.count() > std::numeric_limits<int>::max()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 预算超出 int 范围");
  }
  return OperationResult::success();
}

domain::OperationResult validate_zmq_control_client_config(
    const ZmqControlClientConfig& config) {
  if (config.send_timeout.count() <= 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 发送预算必须为正");
  }
  if (config.receive_high_water_mark == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 接收水位必须为正");
  }
  if (config.receive_high_water_mark >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 接收水位超出 int 范围");
  }
  if (config.send_timeout.count() > std::numeric_limits<int>::max()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 发送预算超出 int 范围");
  }
  return OperationResult::success();
}

struct ZmqControlServer::Impl {
  Impl(gateway::Gateway& gateway_value, ZmqControlServerConfig config_value)
      : gateway(gateway_value), config(std::move(config_value)) {}

  gateway::Gateway& gateway;
  ZmqControlServerConfig config;

  mutable std::mutex mutex;
  std::condition_variable condition;
  std::thread worker;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> running{false};
  bool start_finished = false;
  domain::OperationResult start_result{};
  std::string bound_endpoint;

  void Run();
};

void ZmqControlServer::Impl::Run() {
  domain::OperationResult local_result = OperationResult::success();
  std::string local_endpoint;
  gateway::ConnectionId connection = gateway::kInvalidConnectionId;

  try {
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.set(zmq::sockopt::linger, 0);
    socket.set(zmq::sockopt::rcvhwm, static_cast<int>(config.receive_high_water_mark));
    socket.set(zmq::sockopt::sndhwm, static_cast<int>(config.send_high_water_mark));
    socket.set(zmq::sockopt::rcvtimeo, socket_timeout_millis(config.poll_interval));
    socket.set(zmq::sockopt::sndtimeo, socket_timeout_millis(config.send_timeout));
    socket.bind(config.endpoint);
    local_endpoint = socket.get(zmq::sockopt::last_endpoint);

    {
      std::lock_guard<std::mutex> lock(mutex);
      bound_endpoint = local_endpoint;
      start_result = OperationResult::success();
      start_finished = true;
      running.store(true);
    }
    condition.notify_all();

    connection = gateway.open_connection();
    while (!stop_requested.load()) {
      zmq::message_t message;
      const auto received = socket.recv(message, zmq::recv_flags::none);
      if (!received.has_value()) {
        continue;
      }

      const std::string payload = message.to_string();
      if (connection == gateway::kInvalidConnectionId ||
          !gateway.connection_open(connection)) {
        connection = gateway.open_connection();
      }
      if (connection == gateway::kInvalidConnectionId) {
        const std::string error_reply = make_transport_error_response(
            payload, ErrorCode::kBackendFailure, "控制入口不可用");
        // REP 要求每个收到的请求恰好回一条消息。即使请求无法归属，也必须回一帧；
        // 回一帧空消息会让客户端在协议解码阶段得到确定的非法响应，而不是让 REP
        // 状态机卡死。可归属的请求仍得到结构化错误响应。EAGAIN 表示慢消费者已经
        // 顶住发送水位，本端不再无限等待，关闭 socket 并退出该线程。
        const auto sent = socket.send(zmq::buffer(error_reply), zmq::send_flags::none);
        if (!sent.has_value()) {
          stop_requested.store(true);
          break;
        }
        continue;
      }

      std::string framed = payload;
      framed.push_back('\n');
      const bool open_after_feed = gateway.feed(connection, framed);
      std::string responses;
      while (responses.size() < config.max_response_bytes * 2U) {
        const std::size_t taken =
            gateway.flush(connection, responses, config.max_response_bytes);
        if (taken == 0) {
          break;
        }
      }

      std::string reply = first_line(responses);
      if (reply.empty()) {
        reply = make_transport_error_response(
            payload, ErrorCode::kBackendFailure, "控制入口没有产生响应");
      }
      // 无论请求是否可归属，每个 REP 请求都必须回一帧；空回复由客户端协议层
      // 判定为非法响应，而不是留下未回复的 REP 状态。发送超时说明慢消费者已经把
      // 发送水位顶满，继续循环只会阻塞控制线程；这里明确退出并关闭资源。
      const auto sent = socket.send(zmq::buffer(reply), zmq::send_flags::none);
      if (!sent.has_value()) {
        stop_requested.store(true);
        break;
      }

      if (!open_after_feed) {
        gateway.close_connection(connection, gateway::GatewayCloseReason::kPeerClosed);
        connection = gateway::kInvalidConnectionId;
      }
    }

    if (connection != gateway::kInvalidConnectionId) {
      gateway.close_connection(connection, gateway::GatewayCloseReason::kPeerClosed);
    }
  } catch (const zmq::error_t& error) {
    local_result = OperationResult::failure(ErrorCode::kBackendFailure, error.what());
  } catch (const std::exception& error) {
    local_result = OperationResult::failure(ErrorCode::kBackendFailure, error.what());
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    running.store(false);
    // 线程退出后端点不再可用；保留上一轮端点会让“未运行却还能拿到端点”变成误导证据。
    bound_endpoint.clear();
    if (!start_finished) {
      start_result = local_result.ok() ? OperationResult::failure(
                                             ErrorCode::kBackendFailure,
                                             "ZeroMQ 服务端未完成绑定")
                                       : local_result;
      start_finished = true;
    }
  }
  condition.notify_all();
}

ZmqControlServer::ZmqControlServer(gateway::Gateway& gateway,
                                   ZmqControlServerConfig config)
    : impl_(new Impl(gateway, std::move(config))) {
  const auto validation = validate_zmq_control_server_config(impl_->config);
  if (!validation.ok()) {
    impl_->config = ZmqControlServerConfig{};
  }
}

ZmqControlServer::~ZmqControlServer() {
  if (!impl_) {
    return;
  }
  impl_->stop_requested.store(true);
  try {
    if (impl_->worker.joinable()) {
      impl_->worker.join();
    }
  } catch (...) {
    // 析构路径不传播线程 join 失败；进程回收是最后兜底。正常路径上 join 有界且成功。
  }
}

domain::OperationResult ZmqControlServer::start() {
  if (!impl_) {
    return OperationResult::failure(ErrorCode::kBackendFailure, "ZeroMQ 服务端未初始化");
  }

  std::unique_lock<std::mutex> lock(impl_->mutex);
  if (impl_->running.load() || impl_->worker.joinable()) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted, "ZeroMQ 服务端已经启动");
  }
  impl_->stop_requested.store(false);
  impl_->start_finished = false;
  impl_->start_result = OperationResult::success();
  impl_->bound_endpoint.clear();
  impl_->worker = std::thread(&ZmqControlServer::Impl::Run, impl_.get());

  if (!impl_->condition.wait_for(lock, impl_->config.bind_wait_budget,
                                 [this] { return impl_->start_finished; })) {
    impl_->stop_requested.store(true);
    lock.unlock();
    if (impl_->worker.joinable()) {
      impl_->worker.join();
    }
    return OperationResult::failure(ErrorCode::kTimeout, "ZeroMQ 服务端绑定超时");
  }

  const domain::OperationResult result = impl_->start_result;
  const bool started = result.ok() && impl_->running.load();
  lock.unlock();
  if (!started && impl_->worker.joinable()) {
    impl_->worker.join();
  }
  return result;
}

domain::OperationResult ZmqControlServer::stop() noexcept {
  if (!impl_) {
    return OperationResult::success();
  }
  impl_->stop_requested.store(true);
  try {
    if (impl_->worker.joinable()) {
      impl_->worker.join();
    }
  } catch (const std::exception& error) {
    return OperationResult::failure(ErrorCode::kBackendFailure, error.what());
  } catch (...) {
    return OperationResult::failure(ErrorCode::kBackendFailure, "join 失败");
  }
  return OperationResult::success();
}

bool ZmqControlServer::running() const noexcept {
  return impl_ != nullptr && impl_->running.load();
}

std::string ZmqControlServer::bound_endpoint() const {
  if (!impl_) {
    return {};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->bound_endpoint;
}

struct ZmqControlClient::Impl {
  Impl(std::string endpoint_value, ZmqControlClientConfig config_value)
      : endpoint(std::move(endpoint_value)), config(std::move(config_value)) {}

  std::string endpoint;
  ZmqControlClientConfig config;
  zmq::context_t context{1};
  std::unique_ptr<zmq::socket_t> socket;
  bool closed = false;

  domain::OperationResult Connect() {
    if (closed) {
      return OperationResult::failure(ErrorCode::kAlreadyCompleted, "ZeroMQ 客户端已关闭");
    }
    try {
      socket.reset();
      socket = std::make_unique<zmq::socket_t>(context, zmq::socket_type::req);
      socket->set(zmq::sockopt::linger, 0);
      socket->set(zmq::sockopt::sndtimeo, static_cast<int>(config.send_timeout.count()));
      socket->set(zmq::sockopt::rcvhwm, static_cast<int>(config.receive_high_water_mark));
      socket->connect(endpoint);
      return OperationResult::success();
    } catch (const zmq::error_t& error) {
      socket.reset();
      return OperationResult::failure(ErrorCode::kBackendFailure, error.what());
    }
  }

  void ResetSocket() noexcept {
    try {
      socket.reset();
    } catch (...) {
      // ZeroMQ 析构已在 errno 层面尽力回收；这里不把清理异常传播给调用方。
    }
  }

  std::chrono::milliseconds Remaining(const std::chrono::steady_clock::time_point& start,
                                      std::chrono::milliseconds total) const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (elapsed >= total) {
      return std::chrono::milliseconds::zero();
    }
    return total - elapsed;
  }
};

ZmqControlClient::ZmqControlClient(std::string endpoint,
                                   ZmqControlClientConfig config)
    : impl_(new Impl(std::move(endpoint), std::move(config))) {
  const auto validation = validate_zmq_control_client_config(impl_->config);
  if (!validation.ok()) {
    impl_->config = ZmqControlClientConfig{};
  }
}

ZmqControlClient::~ZmqControlClient() {
  close();
}

domain::OperationResult ZmqControlClient::connect() {
  if (!impl_) {
    return OperationResult::failure(ErrorCode::kBackendFailure, "ZeroMQ 客户端未初始化");
  }
  return impl_->Connect();
}

domain::Result<ControlResponse> ZmqControlClient::call(const ControlRequest& request) {
  if (!impl_ || impl_->closed) {
    return failure<ControlResponse>(ErrorCode::kAlreadyCompleted, "ZeroMQ 客户端已关闭");
  }
  const auto validation = protocol::validate_request(request);
  if (!validation.ok()) {
    return failure<ControlResponse>(validation.error.code, validation.error.message);
  }
  const auto encoded = protocol::encode_request(request);
  if (!encoded.ok()) {
    return failure<ControlResponse>(encoded.error.code, encoded.error.message);
  }
  if (!impl_->socket) {
    const auto connected = impl_->Connect();
    if (!connected.ok()) {
      return failure<ControlResponse>(connected.error.code, connected.error.message);
    }
  }

  const auto start = std::chrono::steady_clock::now();
  const auto total = request.deadline;
  try {
    const auto before_send = impl_->Remaining(start, total);
    if (before_send <= std::chrono::milliseconds::zero()) {
      impl_->ResetSocket();
      return failure<ControlResponse>(ErrorCode::kTimeout, "请求预算在发送前耗尽");
    }
    impl_->socket->set(zmq::sockopt::sndtimeo, socket_timeout_millis(before_send));
    const auto sent =
        impl_->socket->send(zmq::buffer(*encoded.value), zmq::send_flags::none);
    if (!sent.has_value()) {
      impl_->ResetSocket();
      return failure<ControlResponse>(ErrorCode::kTimeout, "发送响应超时");
    }

    const auto before_receive = impl_->Remaining(start, total);
    if (before_receive <= std::chrono::milliseconds::zero()) {
      impl_->ResetSocket();
      return failure<ControlResponse>(ErrorCode::kTimeout, "请求预算在接收前耗尽");
    }
    impl_->socket->set(zmq::sockopt::rcvtimeo, socket_timeout_millis(before_receive));
    zmq::message_t message;
    const auto received = impl_->socket->recv(message, zmq::recv_flags::none);
    if (!received.has_value()) {
      impl_->ResetSocket();
      return failure<ControlResponse>(ErrorCode::kTimeout, "等待控制响应超时");
    }

    const auto response = protocol::decode_response(message.to_string());
    if (!response.ok()) {
      return failure<ControlResponse>(response.error.code, response.error.message);
    }
    if (response.value->request_id != request.request_id) {
      return failure<ControlResponse>(ErrorCode::kInvalidInput,
                                      "控制响应与请求标识不一致");
    }
    return response;
  } catch (const zmq::error_t& error) {
    impl_->ResetSocket();
    if (error.num() == EAGAIN || error.num() == EINTR) {
      return failure<ControlResponse>(ErrorCode::kTimeout, error.what());
    }
    return failure<ControlResponse>(ErrorCode::kBackendFailure, error.what());
  }
}

void ZmqControlClient::close() noexcept {
  if (!impl_ || impl_->closed) {
    return;
  }
  impl_->closed = true;
  impl_->ResetSocket();
}

bool ZmqControlClient::connected() const noexcept {
  return impl_ != nullptr && !impl_->closed && impl_->socket != nullptr;
}

}  // namespace nexweave::transport

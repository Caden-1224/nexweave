#include "remote_session_proxy.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

namespace nexweave::transport {
namespace {

using domain::ErrorCode;
using domain::OperationResult;
using protocol::DataEvent;
using runtime::ChildProcessExit;
using runtime::ChildProcessIdentity;

std::string ReadEndpointFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::string();
  }
  std::string endpoint;
  std::getline(file, endpoint);
  while (!endpoint.empty() &&
         (endpoint.back() == '\r' || endpoint.back() == '\n' || endpoint.back() == ' ')) {
    endpoint.pop_back();
  }
  return endpoint;
}

}  // namespace

domain::OperationResult validate_remote_session_proxy_config(
    const RemoteSessionProxyConfig& config) {
  const auto child_valid = runtime::validate_child_process_spec(config.child);
  if (!child_valid.ok()) {
    return child_valid;
  }
  const auto config_valid = runtime::validate_child_process_config(config.child_config);
  if (!config_valid.ok()) {
    return config_valid;
  }
  if (config.endpoint_file.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "端点文件路径不能为空");
  }
  if (config.pump_interval.count() <= 0 || config.ready_timeout.count() <= 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "代理等待预算必须为正");
  }
  if (config.max_pending_input_events == 0 || config.max_pending_output_events == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "代理队列容量必须为正");
  }
  return validate_zmq_data_config(config.data);
}

struct RemoteSessionProxy::Impl {
  explicit Impl(RemoteSessionProxyConfig config_value)
      : config(std::move(config_value)),
        channel(config.data) {}

  RemoteSessionProxyConfig config;
  runtime::ChildProcess child{config.child_config};
  ZmqDataChannel channel;
  std::atomic<bool> running{false};
  std::atomic<bool> stop_requested{false};
  std::thread io_thread;

  mutable std::mutex mutex;
  std::condition_variable input_ready;
  std::condition_variable output_ready;
  std::deque<runtime::InputStreamEvent> inbound;
  std::deque<DataEvent> outbound;
  domain::Error error{};
  RemoteSessionProxyStats stats{};
  bool stream_started = false;
  bool stream_ended = false;
  std::string stream_id;
  std::uint64_t generation = 0;
  std::uint64_t next_sequence = 0;

  domain::OperationResult Start();
  OperationResult Stop() noexcept;
  domain::OperationResult QueueStart(std::string id, std::uint64_t generation_value);
  domain::OperationResult QueueFrame(const domain::AudioFrame& frame);
  domain::OperationResult QueueEnd(std::size_t valid_samples,
                                  std::optional<domain::AudioFrame> tail);
  domain::OperationResult QueueCancel();
  std::size_t ReceiveEvents(std::vector<DataEvent>& out, std::size_t max_events,
                            std::chrono::milliseconds timeout);
  void IoLoop();
  bool PushInputLocked(const runtime::InputStreamEvent& event);
  void RecordError(const domain::Error& error_value);
};

domain::OperationResult RemoteSessionProxy::Impl::Start() {
  bool expected = false;
  if (!running.compare_exchange_strong(expected, true)) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted, "远端代理已经启动");
  }
  stop_requested.store(false);
  {
    const std::lock_guard<std::mutex> lock(mutex);
    inbound.clear();
    outbound.clear();
    error = domain::Error{};
    stats = RemoteSessionProxyStats{};
    stream_started = false;
    stream_ended = false;
    stream_id.clear();
    generation = 0;
    next_sequence = 0;
  }

  std::remove(config.endpoint_file.c_str());

  runtime::ChildProcessSpec spec = config.child;
  spec.environment.push_back(std::string(kRemoteSessionEndpointFileEnvironment) + "=" +
                             config.endpoint_file);
  const auto started = child.start(spec);
  if (!started.ok()) {
    running.store(false);
    RecordError(started.error);
    return OperationResult{started.error};
  }

  const std::string endpoint = ReadEndpointFile(config.endpoint_file);
  if (endpoint.empty()) {
    const OperationResult failed =
        OperationResult::failure(ErrorCode::kBackendFailure, "子进程没有写出数据面端点");
    RecordError(failed.error);
    child.stop(started.value.value());
    running.store(false);
    return failed;
  }

  const auto connected = channel.connect(endpoint);
  if (!connected.ok()) {
    RecordError(connected.error);
    child.stop(started.value.value());
    running.store(false);
    return connected;
  }
  const auto ready = channel.wait_ready(config.ready_timeout);
  if (!ready.ok()) {
    RecordError(ready.error);
    channel.close();
    child.stop(started.value.value());
    running.store(false);
    return ready;
  }

  io_thread = std::thread([this] { IoLoop(); });
  return OperationResult::success();
}

OperationResult RemoteSessionProxy::Impl::Stop() noexcept {
  try {
    if (!running.load()) {
      return OperationResult::success();
    }
    // 先让已经在途的输入有机会送达，再放入取消事件；停止标志只阻止新的 queue_* 调用。
    (void)QueueCancel();
    stop_requested.store(true);
    input_ready.notify_all();
    if (io_thread.joinable()) {
      io_thread.join();
    }
    channel.close();
    const auto stopped = child.stop();
    running.store(false);
    if (!stopped.ok()) {
      RecordError(stopped.error);
      return OperationResult{stopped.error};
    }
    return OperationResult::success();
  } catch (...) {
    running.store(false);
    return OperationResult::failure(ErrorCode::kBackendFailure,
                                    "停止远端代理时发生异常");
  }
}

bool RemoteSessionProxy::Impl::PushInputLocked(const runtime::InputStreamEvent& event) {
  if (stop_requested.load()) {
    return false;
  }
  if (inbound.size() >= config.max_pending_input_events) {
    ++stats.input_queue_full;
    return false;
  }
  inbound.push_back(event);
  input_ready.notify_all();
  return true;
}

domain::OperationResult RemoteSessionProxy::Impl::QueueStart(std::string id,
                                                             std::uint64_t generation_value) {
  if (id.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "输入流标识不能为空");
  }
  runtime::InputStreamEvent event;
  event.stream_id = std::move(id);
  event.generation = generation_value;
  event.sequence = 0;
  event.kind = runtime::InputEventKind::kStart;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    if (!running.load() || stop_requested.load()) {
      return OperationResult::failure(ErrorCode::kAlreadyCompleted, "远端代理尚未运行");
    }
    if (stream_started && !stream_ended) {
      return OperationResult::failure(ErrorCode::kAlreadyCompleted, "输入流已经开始");
    }
    if (!PushInputLocked(event)) {
      return OperationResult::failure(ErrorCode::kBackendFailure, "输入事件队列已满");
    }
    stream_started = true;
    stream_ended = false;
    stream_id = event.stream_id;
    generation = generation_value;
    next_sequence = 1;
    ++stats.queued_input_events;
  }
  return OperationResult::success();
}

domain::OperationResult RemoteSessionProxy::Impl::QueueFrame(
    const domain::AudioFrame& frame) {
  const auto valid = domain::validate_audio_frame(frame);
  if (!valid.ok()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "只接受合法固定音频帧");
  }
  runtime::InputStreamEvent event;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    if (!running.load() || stop_requested.load()) {
      return OperationResult::failure(ErrorCode::kAlreadyCompleted, "远端代理尚未运行");
    }
    if (!stream_started || stream_ended) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "输入流尚未开始或已经结束");
    }
    event.stream_id = stream_id;
    event.generation = generation;
    event.sequence = next_sequence;
    event.kind = runtime::InputEventKind::kFrame;
    event.frame = frame;
    event.valid_samples = domain::kAudioFrameSamples;
    if (!PushInputLocked(event)) {
      return OperationResult::failure(ErrorCode::kBackendFailure, "输入事件队列已满");
    }
    ++next_sequence;
    ++stats.queued_input_events;
  }
  return OperationResult::success();
}

domain::OperationResult RemoteSessionProxy::Impl::QueueEnd(
    std::size_t valid_samples, std::optional<domain::AudioFrame> tail) {
  if (valid_samples == 0 && tail.has_value()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "零样本结束不能携带尾帧");
  }
  if (valid_samples > domain::kAudioFrameSamples) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "结束有效样本数越界");
  }
  if (valid_samples > 0) {
    if (!tail.has_value()) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "短尾结束必须携带补零尾帧");
    }
    const auto valid = domain::validate_audio_frame(*tail);
    if (!valid.ok()) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "补零尾帧不符合音频合同");
    }
  }

  runtime::InputStreamEvent event;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    if (!running.load() || stop_requested.load()) {
      return OperationResult::failure(ErrorCode::kAlreadyCompleted, "远端代理尚未运行");
    }
    if (!stream_started || stream_ended) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "输入流尚未开始或已经结束");
    }
    event.stream_id = stream_id;
    event.generation = generation;
    event.sequence = next_sequence;
    event.kind = runtime::InputEventKind::kEnd;
    event.valid_samples = valid_samples;
    if (tail.has_value()) {
      event.frame = std::move(*tail);
    }
    if (!PushInputLocked(event)) {
      return OperationResult::failure(ErrorCode::kBackendFailure, "输入事件队列已满");
    }
    stream_ended = true;
    ++next_sequence;
    ++stats.queued_input_events;
  }
  return OperationResult::success();
}

domain::OperationResult RemoteSessionProxy::Impl::QueueCancel() {
  runtime::InputStreamEvent event;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    if (!stream_started || stream_ended) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "没有可取消的输入流");
    }
    event.stream_id = stream_id;
    event.generation = generation;
    event.sequence = next_sequence;
    event.kind = runtime::InputEventKind::kCancel;
    if (!PushInputLocked(event)) {
      return OperationResult::failure(ErrorCode::kBackendFailure, "输入事件队列已满");
    }
    stream_ended = true;
    ++next_sequence;
    ++stats.queued_input_events;
  }
  return OperationResult::success();
}

std::size_t RemoteSessionProxy::Impl::ReceiveEvents(
    std::vector<DataEvent>& out, std::size_t max_events,
    std::chrono::milliseconds timeout) {
  if (max_events == 0) {
    return 0;
  }
  std::unique_lock<std::mutex> lock(mutex);
  output_ready.wait_for(lock, timeout, [this] {
    return !outbound.empty() || !error.ok() || stop_requested.load();
  });
  std::size_t count = 0;
  while (count < max_events && !outbound.empty()) {
    out.push_back(std::move(outbound.front()));
    outbound.pop_front();
    ++count;
  }
  return count;
}

void RemoteSessionProxy::Impl::IoLoop() {
  while (true) {
    runtime::InputStreamEvent event;
    bool has_input = false;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (!inbound.empty()) {
        event = std::move(inbound.front());
        inbound.pop_front();
        has_input = true;
      } else if (stop_requested.load()) {
        break;
      }
    }
    if (has_input) {
      const auto sent = channel.send_input(event);
      if (!sent.ok()) {
        RecordError(sent.error);
        {
          const std::lock_guard<std::mutex> lock(mutex);
          ++stats.send_failures;
        }
        child.request_stop();
        break;
      }
      {
        const std::lock_guard<std::mutex> lock(mutex);
        ++stats.sent_input_events;
      }
      continue;
    }

    const auto received = channel.receive_output(config.pump_interval);
    if (received.ok()) {
      bool overflow = false;
      {
        const std::lock_guard<std::mutex> lock(mutex);
        if (outbound.size() >= config.max_pending_output_events) {
          ++stats.output_queue_full;
          if (error.ok()) {
            error = domain::Error{ErrorCode::kBackendFailure, "远端输出队列已满"};
          }
          overflow = true;
        } else {
          outbound.push_back(received.value.value());
          ++stats.received_output_events;
        }
      }
      if (overflow) {
        child.request_stop();
        break;
      }
      output_ready.notify_all();
      continue;
    }

    if (received.error.code == ErrorCode::kTimeout) {
      const std::lock_guard<std::mutex> lock(mutex);
      ++stats.receive_timeouts;
      continue;
    }

    RecordError(received.error);
    child.request_stop();
    break;
  }
  output_ready.notify_all();
}

void RemoteSessionProxy::Impl::RecordError(const domain::Error& error_value) {
  const std::lock_guard<std::mutex> lock(mutex);
  if (error.ok()) {
    error = error_value;
  }
}

RemoteSessionProxy::RemoteSessionProxy(RemoteSessionProxyConfig config) {
  if (!validate_remote_session_proxy_config(config).ok()) {
    config = RemoteSessionProxyConfig{};
  }
  impl_ = std::make_unique<Impl>(std::move(config));
}

RemoteSessionProxy::~RemoteSessionProxy() {
  stop();
}

domain::OperationResult RemoteSessionProxy::start() {
  return impl_->Start();
}

domain::OperationResult RemoteSessionProxy::stop() noexcept {
  return impl_->Stop();
}

domain::OperationResult RemoteSessionProxy::queue_start_stream(std::string stream_id,
                                                               std::uint64_t generation) {
  if (impl_ == nullptr) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "代理尚未构造");
  }
  return impl_->QueueStart(std::move(stream_id), generation);
}

domain::OperationResult RemoteSessionProxy::queue_frame(const domain::AudioFrame& frame) {
  if (impl_ == nullptr) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "代理尚未构造");
  }
  return impl_->QueueFrame(frame);
}

domain::OperationResult RemoteSessionProxy::queue_end_stream(
    std::size_t valid_samples, std::optional<domain::AudioFrame> tail) {
  if (impl_ == nullptr) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "代理尚未构造");
  }
  return impl_->QueueEnd(valid_samples, std::move(tail));
}

domain::OperationResult RemoteSessionProxy::queue_cancel_stream() {
  if (impl_ == nullptr) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "代理尚未构造");
  }
  return impl_->QueueCancel();
}

std::size_t RemoteSessionProxy::receive_events(std::vector<protocol::DataEvent>& out,
                                               std::size_t max_events,
                                               std::chrono::milliseconds timeout) {
  if (impl_ == nullptr) {
    return 0;
  }
  return impl_->ReceiveEvents(out, max_events, timeout);
}

domain::Error RemoteSessionProxy::last_error() const {
  if (impl_ == nullptr) {
    return domain::Error{};
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->error;
}

bool RemoteSessionProxy::running() const noexcept {
  return impl_ != nullptr && impl_->running.load();
}

RemoteSessionProxyStats RemoteSessionProxy::stats() const {
  if (impl_ == nullptr) {
    return RemoteSessionProxyStats{};
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->stats;
}

}  // namespace nexweave::transport

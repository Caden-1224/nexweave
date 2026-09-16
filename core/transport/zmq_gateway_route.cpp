#include "zmq_gateway_route.hpp"

#include <exception>
#include <utility>

namespace nexweave::transport {

ZmqControlRoute::ZmqControlRoute(std::string endpoint, ZmqControlClientConfig config)
    : endpoint_(std::move(endpoint)), config_(std::move(config)) {
  if (!validate_zmq_control_client_config(config_).ok()) {
    config_ = ZmqControlClientConfig{};
  }
}

ZmqControlRoute::~ZmqControlRoute() = default;

domain::OperationResult ZmqControlRoute::connect() {
  if (endpoint_.empty()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "控制路由端点不能为空");
  }
  if (client_ != nullptr && client_->connected()) {
    return domain::OperationResult::success();
  }
  client_ = std::make_unique<ZmqControlClient>(endpoint_, config_);
  const domain::OperationResult connected = client_->connect();
  if (!connected.ok()) {
    client_.reset();
    return connected;
  }
  return domain::OperationResult::success();
}

void ZmqControlRoute::close() noexcept {
  if (client_ != nullptr) {
    client_->close();
    client_.reset();
  }
}

bool ZmqControlRoute::connected() const noexcept {
  return client_ != nullptr && client_->connected();
}

domain::Result<protocol::ControlResponse> ZmqControlRoute::call(
    const protocol::ControlRequest& request) {
  if (client_ == nullptr || !client_->connected()) {
    return domain::Result<protocol::ControlResponse>::failure(
        domain::ErrorCode::kBackendFailure, "控制路由尚未连接");
  }
  try {
    return client_->call(request);
  } catch (const std::exception& error) {
    return domain::Result<protocol::ControlResponse>::failure(
        domain::ErrorCode::kBackendFailure,
        std::string("控制路由调用异常: ") + error.what());
  } catch (...) {
    return domain::Result<protocol::ControlResponse>::failure(
        domain::ErrorCode::kBackendFailure, "控制路由调用发生未知异常");
  }
}

std::size_t ZmqControlRoute::poll_events(gateway::ControlRouteOwner owner,
                                         std::vector<protocol::DataEvent>& out,
                                         std::size_t max_events) {
  (void)owner;
  (void)out;
  (void)max_events;
  return 0;
}

}  // namespace nexweave::transport

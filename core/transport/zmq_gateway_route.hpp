// ZeroMQ 控制路由适配器：把 Gateway 的控制请求转发给独立进程中的 Supervisor。
//
// 职责与边界
// ----------
// 本类只实现 `gateway::IControlRoute` 的 ZeroMQ 传输部分：连接、发送一个请求、等待一个
// 响应、按 deadline 收敛。它不解析外部 NDJSON，不替代 Gateway 的分帧、幂等、连接账目和
// 有界发送，也不创建或解释 Supervisor/Session 的业务状态。控制请求和响应仍是既有 v1 值
// 对象，因此外部协议不因拆进程而改变。
//
// 并发与连接所有权
// ----------------
// Gateway 路由模式会从多个工作线程并发调用 `call()`，因此本类不能共享一个有状态的
// `ZmqControlClient` socket。每次 `call()` 在调用线程内创建短生命周期客户端，独立完成
// connect → send → recv → close；慢转发因此只占用一个工作线程，不会阻塞其他控制请求。
// 代价是每条请求有一次本地端点连接建立开销；真实部署若需要长连接池，应由后续 profile
// 适配在不改变本接口语义的前提下替换实现。
//
// 数据事件
// --------
// 当前控制面请求/响应不含数据事件；`poll_events()` 返回 0。事件回流由后续多进程 Session
// 适配接入，但接口已经在这里占位，Gateway 无需再改公共行为。
//
// 资源
// ----
// `connect()` 只校验端点并置就绪标志，不创建 socket；`call()` 创建的客户端在返回前释放
// context/socket。`close()` 使后续 `call()` 返回结构化失败，幂等、不抛异常。
#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include "../gateway/control_route.hpp"
#include "../protocol/control_rpc.hpp"
#include "zmq_control.hpp"

namespace nexweave::transport {

// ZeroMQ 控制路由。构造只保存端点与客户端配置；connect() 只做端点校验，call() 每次创建
// 独立客户端，因此本类可被多个工作线程并发使用。
class ZmqControlRoute final : public gateway::IControlRoute {
 public:
  explicit ZmqControlRoute(std::string endpoint,
                           ZmqControlClientConfig config = {});
  ~ZmqControlRoute() override;

  ZmqControlRoute(const ZmqControlRoute&) = delete;
  ZmqControlRoute& operator=(const ZmqControlRoute&) = delete;

  // 校验端点并置为可调用。重复调用幂等；端点为空的配置返回 kInvalidInput。
  domain::OperationResult connect();

  // 清除就绪标志；已经创建并按次释放的客户端不受影响。幂等、不抛异常。
  void close() noexcept;

  bool connected() const noexcept;

  // 并发安全地转发一次控制请求。每次调用创建独立客户端，因此多个慢转发互不占用对方的
  // socket 状态；远端不可达、deadline 超时或响应非法时返回结构化错误，不伪造成功。
  domain::Result<protocol::ControlResponse> call(
      const protocol::ControlRequest& request) override;

  // 当前控制面没有事件通道；返回 0。后续事件适配会在这里接入，不改变调用约定。
  std::size_t poll_events(gateway::ControlRouteOwner owner,
                          std::vector<protocol::DataEvent>& out,
                          std::size_t max_events) override;

 private:
  std::string endpoint_;
  ZmqControlClientConfig config_;
  std::atomic<bool> connected_{false};
};

}  // namespace nexweave::transport

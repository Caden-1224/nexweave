// ZeroMQ 控制路由适配器：把 Gateway 的控制请求转发给独立进程中的 Supervisor。
//
// 职责与边界
// ----------
// 本类只实现 `gateway::IControlRoute` 的 ZeroMQ 传输部分：连接、发送一个请求、等待一个
// 响应、按 deadline 收敛。它不解析外部 NDJSON，不替代 Gateway 的分帧、幂等、连接账目和
// 有界发送，也不创建或解释 Supervisor/Session 的业务状态。控制请求和响应仍是既有 v1 值
// 对象，因此外部协议不因拆进程而改变。
//
// 进程生命周期
// ------------
// 路由只负责“连到某个 Supervisor 控制端点”；启动、就绪等待、停止升级和身份隔离由
// `runtime::ChildProcess` 负责。两者由上层 profile 组合，避免路由对象同时承担进程管理和
// 协议传输两种生命周期。
//
// 数据事件
// --------
// 当前控制面请求/响应不含数据事件；`poll_events()` 返回 0。事件回流由后续多进程 Session
// 适配接入，但接口已经在这里占位，Gateway 无需再改公共行为。
//
// 线程与资源
// ----------
// `ZmqControlClient` 不是线程安全的，因此本类也必须由 `Gateway` 的同一个驱动线程串行使用。
// 本类拥有 `ZmqControlClient`；`connect()` 后可用，`close()` 或析构时释放 socket/context。
// `call()` 的等待由请求自带 deadline 与客户端 send_timeout 共同约束，不会无限等待。
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "../gateway/control_route.hpp"
#include "../protocol/control_rpc.hpp"
#include "zmq_control.hpp"

namespace nexweave::transport {

// ZeroMQ 控制路由。构造只保存端点与客户端配置，不建立连接；connect() 才创建 socket。
class ZmqControlRoute final : public gateway::IControlRoute {
 public:
  explicit ZmqControlRoute(std::string endpoint,
                           ZmqControlClientConfig config = {});
  ~ZmqControlRoute() override;

  ZmqControlRoute(const ZmqControlRoute&) = delete;
  ZmqControlRoute& operator=(const ZmqControlRoute&) = delete;

  // 建立/重建到远端的 REQ socket。重复调用在已连接时成功返回；配置或端点非法时返回
  // 结构化错误且不留下半连接对象。成功后 connected() 为真。
  domain::OperationResult connect();

  // 幂等关闭本路由的 socket 与客户端对象。关闭后可再次 connect()。
  void close() noexcept;

  bool connected() const noexcept;

  // 转发一次控制请求。成功返回远端 ControlResponse；远端不可达、超时或响应非法时返回
  // kTimeout/kBackendFailure 等结构化错误。本方法可能阻塞，但不会超过请求 deadline 加
  // 客户端单次发送预算；不得在持有 Gateway 互斥量的路径上调用。
  domain::Result<protocol::ControlResponse> call(
      const protocol::ControlRequest& request) override;

  // 当前控制面没有事件通道；返回 0。后续事件适配会在这里接入，不改变本接口的调用约定。
  std::size_t poll_events(gateway::ControlRouteOwner owner,
                          std::vector<protocol::DataEvent>& out,
                          std::size_t max_events) override;

 private:
  std::string endpoint_;
  ZmqControlClientConfig config_;
  std::unique_ptr<ZmqControlClient> client_;
};

}  // namespace nexweave::transport

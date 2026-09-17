// 远端 Session 控制路由：把 Gateway 的控制请求导到独立 Session 进程与数据面代理。
//
// 职责与边界
// ----------
// 本类实现 gateway::IControlRoute，但执行体不是远端 Gateway，而是一个由本类拥有的
// RemoteSessionProxy。start 请求负责建立独立 Session 子进程并启动输入生产者；query/cancel/exit
// 操作远端会话状态；poll_events 把数据面回传的文本、PCM 和终态交回本地 Gateway。
// 它不解析外部 NDJSON，不替 Gateway 做幂等，也不把 ZeroMQ 或进程句柄传入 Session。
//
// 单活跃与忙碌拒绝
// ----------------
// 一台设备同一时刻只允许一个 Session。本类在 start 时检查路由状态，已经在途时返回 kBusy，
// 不排队、不替换。会话终态被 poll_events 取走后进入 completed，下一次 start 会先清理上一轮
// 子进程与输入线程，再建立全新会话；旧 request 和旧 generation 不会命中新会话。
//
// 输入生产与取消
// --------------
// 输入由调用方注入的 IAudioSource 工厂提供。生产者线程在 start 成功后按帧读取源，逐帧调用
// RemoteSessionProxy::queue_frame；源 EOF 时发送结束，源取消或失败时发送取消。取消请求只
// 置停止标志、取消音频源并投递数据面取消，不等待整段输入结束。exit 会等待输入线程退出并
// 回收子进程。
//
// 资源与线程
// ----------
// 本类创建：一个 RemoteSessionProxy、一个输入生产者线程和若干状态字段。RemoteSessionProxy
// 自己拥有子进程、数据通道和 I/O 线程；输入源由 factory 创建并由生产者线程拥有，close 在
// 线程退出前调用。所有控制入口可由 Gateway 的多个工作线程并发调用，状态由互斥量线性化。
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "../capability/backend.hpp"
#include "../gateway/control_route.hpp"
#include "../protocol/data_event.hpp"
#include "remote_session_proxy.hpp"

namespace nexweave::transport {

// 远端 Session 路由配置。proxy 配置描述子进程与数据面；input_factory 为每次 start 建立新的
// 音频源，避免上一会话的读取游标或取消状态漏进下一会话；frame_interval 控制逐帧上行节奏。
struct RemoteSessionRouteConfig {
  RemoteSessionProxyConfig proxy{};
  std::function<std::unique_ptr<capability::IAudioSource>()> input_factory;
  std::string stream_id = "linux-profile";
  std::chrono::milliseconds frame_interval{0};
};

// 配置校验：proxy 配置必须有效，input_factory 和 stream_id 必须非空，frame_interval 不得为负。
domain::OperationResult validate_remote_session_route_config(
    const RemoteSessionRouteConfig& config);

// 远端 Session 控制路由。构造只保存配置和创建代理对象，不启动子进程、不创建输入线程。
class RemoteSessionRoute final : public gateway::IControlRoute {
 public:
  explicit RemoteSessionRoute(RemoteSessionRouteConfig config = {});
  ~RemoteSessionRoute() override;

  RemoteSessionRoute(const RemoteSessionRoute&) = delete;
  RemoteSessionRoute& operator=(const RemoteSessionRoute&) = delete;

  // 执行 start/query/cancel/exit。start 成功返回后输入生产者已经启动；query/cancel 立即返回；
  // exit 等待输入线程与子进程收敛。重复 request 的幂等由 Gateway 负责，本类只回答当前状态。
  domain::Result<protocol::ControlResponse> call(
      const protocol::ControlRequest& request) override;

  // 非阻塞取出当前会话已到达的数据事件；终态事件出现后状态进入 completed，下一次 start
  // 会清理资源。没有会话或当前没有事件时返回 0。
  std::size_t poll_events(gateway::ControlRouteOwner owner,
                          std::vector<protocol::DataEvent>& out,
                          std::size_t max_events) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::transport

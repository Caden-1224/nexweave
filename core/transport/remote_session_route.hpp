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
  // 取消受理后等待远端产生唯一终态的预算。预算内没有终态时，poll_events() 会交付一条
  // kTimeout 终态并请求子进程停止；它不把“响应成功”当成“后端已经停止”。必须为正。
  std::chrono::milliseconds cancel_timeout{5000};
};

// 配置校验：proxy 配置必须有效，input_factory 和 stream_id 必须非空，frame_interval 不得为负，
// cancel_timeout 必须为正。
domain::OperationResult validate_remote_session_route_config(
    const RemoteSessionRouteConfig& config);

// 远端路由只读账目快照。路由自身计数在对象生命周期内累加；transport_stale_events_filtered
// 与 discarded_output_events 来自当前或最近一次代理 start，代理重新 start 时清零。duration
// 字段记录最近一次已受理取消的各阶段耗时；值为 0 表示该阶段没有完成，而不是“立即完成”。
// cancellation_pending 与 terminal_delivered 描述快照瞬间的当前 start。
struct RemoteSessionRouteStats {
  std::uint64_t starts_accepted = 0;
  std::uint64_t cancel_requests = 0;
  std::uint64_t duplicate_cancel_requests = 0;
  // 路由层已经取回、并按代际水位或终态水位丢弃的旧输出事件数。终态交付后继续非阻塞
  // 排空代理队列和 socket，使“在途旧事件被拒绝”成为可观察事实；它不是对端待发送量。
  std::uint64_t stale_events_filtered = 0;
  // 传输层在 socket 接收路径判定为旧代际或重复终态并丢弃的事件数；与路由层过滤分开记账。
  std::uint64_t transport_stale_events_filtered = 0;
  // 终态交付或取消超时后，从代理输出队列主动丢弃的事件数；只代表软件队列，不代表 socket。
  std::uint64_t discarded_output_events = 0;
  std::uint64_t terminal_events_delivered = 0;
  std::uint64_t cancel_timeouts = 0;
  // 取消受理到当前轮次唯一终态交付。取消场景下，远端服务端只在会话收尾后发送该终态，
  // 因此它是跨进程观测的上界，不等同于 SDK 内部计算或设备实际静默已经停止。
  std::chrono::nanoseconds last_cancel_accept_to_terminal{0};
  // 取消受理到“后端停止”的协议上界观察时间；正常取消时与远端终态同一事件，超时路径保持 0，
  // 表示预算内没有收到后端停止证据。真实 SDK 停止时间仍需板端测量。
  std::chrono::nanoseconds last_cancel_accept_to_backend_stop{0};
  // 取消受理到本地输出队列完成丢弃的时间。终态批次中剩余的旧事件和后续 socket 在途事件
  // 都会被清掉；该时刻只表达软件队列，不表达设备播放缓冲。
  std::chrono::nanoseconds last_cancel_accept_to_queue_clear{0};
  // 取消受理到路由完成本轮收敛（唯一终态、旧输出封锁、本地队列清理）的时间。它不承诺
  // 后端进程已在同刻退出；进程回收由下一次 start/exit 或析构的停止路径负责。
  std::chrono::nanoseconds last_cancel_accept_to_total{0};
  bool cancellation_pending = false;
  bool terminal_delivered = false;
};

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
  // 会清理资源。每个 start 生命周期只交付一个 end=true 终态；终态之后的旧事件会继续从代理
  // 队列和 socket 非阻塞取回并丢弃，计入 stale_events_filtered 或 transport_stale_events_filtered。
  // 取消超时会在下一次 poll_events() 交付 kTimeout 终态。
  std::size_t poll_events(gateway::ControlRouteOwner owner,
                          std::vector<protocol::DataEvent>& out,
                          std::size_t max_events) override;

  // 路由账目快照。可跨线程调用，返回副本；不阻塞、不驱动 I/O。
  RemoteSessionRouteStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::transport

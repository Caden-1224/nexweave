// 远端会话代理：在父进程管理独立 Session 子进程，并驱动数据面输入/输出。
//
// 职责与边界
// ----------
// 本类把一个已经实现好“队列音频源 + SessionApp + 远端会话服务端”的可执行文件当作黑盒：
// 启动子进程、等待显式就绪、连接数据面端点，然后把 InputStreamEvent 有界地送上去，把
// DataEvent 有界地收回来。它不解析业务文本、不运行会话、不访问模型或设备，也不把 ZeroMQ
// 或 POSIX 进程句柄泄漏给 Session。
//
// 独立推进
// --------
// 一个内部 I/O 线程独占 ZmqDataChannel：生产线程通过 queue_* 把输入事件放进有界队列，消费
// 线程通过 receive_events() 取出输出事件，两者只共享互斥量保护的队列，不直接操作 socket。
// 因此输入生产、输出接收和控制取消可以分别从不同线程调用，不会把慢接收端变成发送端阻塞。
//
// 取消与退出
// ----------
// queue_cancel_stream() 构造 kCancel 输入事件；I/O 线程先发送已经排队的输入，再发送取消，
// 服务端因此能按数据面语义终止会话。stop() 会请求取消、停止子进程并回收全部资源；重复调用
// 幂等。父进程退出或析构不会留下未回收的子进程。
//
// 资源与失败
// ----------
// 代理创建并拥有：一个 ChildProcess、一个数据面通道和一个 I/O 线程。端点文件由子进程按
// 环境变量约定写出，代理只读不写；文件路径由调用方提供，避免代理偷偷选择全局临时位置。
// 连接失败、ready 超时、队列满、发送失败和子进程退出都以结构化错误结束，已排队输入不会
// 被静默丢弃：调用方可以先看 stats()/last_error() 再决定重建或上报。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../domain/error.hpp"
#include "../protocol/data_event.hpp"
#include "../runtime/child_process.hpp"
#include "../runtime/interaction_contract.hpp"
#include "zmq_data.hpp"

namespace nexweave::transport {

// 代理与子进程之间的私有就绪约定：子进程在这一环境变量指向的文件中写入数据面实际端点，
// 并在写完后才调用 runtime::notify_parent_ready()。代理读取该文件并连接。
inline constexpr char kRemoteSessionEndpointFileEnvironment[] =
    "NEXWEAVE_REMOTE_SESSION_ENDPOINT_FILE";

// 远端会话代理配置。传输容量与等待预算显式可配；非法值构造时整份回退默认值。
struct RemoteSessionProxyConfig {
  // 子进程启动参数。executable 必填；代理会向 environment 追加端点文件变量，调用方无需
  // 自己拼路径。子进程必须按上面的私有约定写端点并通知就绪。
  runtime::ChildProcessSpec child{};
  runtime::ChildProcessConfig child_config{};
  // 子进程写出的端点文件路径。代理启动前删除旧文件，避免把过期端点当成新进程就绪。
  std::string endpoint_file;
  ZmqDataConfig data{};
  // 读取端点到完成数据面 ready 握手的预算；必须为正。
  std::chrono::milliseconds ready_timeout{5000};
  // 无输出事件时的接收轮询粒度；必须为正。
  std::chrono::milliseconds pump_interval{10};
  // 尚未发送的输入事件上限与尚未被调用方取走的输出事件上限。满队列不阻塞；输入队列满时
  // queue_* 返回 kBackendFailure，输出队列满时代理进入显式失败并停止子进程。
  std::size_t max_pending_input_events = 64;
  std::size_t max_pending_output_events = 1024;
  // 终态缓存容量与保留期限。终态即使没有及时从输出队列取走，也可以在期限内查询；
  // 超过期限查询返回明确失败。两者都必须为正。
  std::size_t terminal_cache_capacity = 8;
  std::chrono::milliseconds terminal_retention{30000};
};

// 配置校验：进程参数、端点文件路径、预算和容量都必须有效。只读，不启动进程、不创建 socket。
domain::OperationResult validate_remote_session_proxy_config(
    const RemoteSessionProxyConfig& config);

// 代理只读账目快照。同一次 start 生命周期内计数只增不减；start 成功开始新的
// 子进程会话时清零，避免上一会话的峰值和失败被误读成本会话事实。
struct RemoteSessionProxyStats {
  std::uint64_t queued_input_events = 0;
  std::uint64_t sent_input_events = 0;
  std::uint64_t received_output_events = 0;
  std::uint64_t input_queue_full = 0;
  std::uint64_t output_queue_full = 0;
  std::uint64_t send_failures = 0;
  std::uint64_t receive_timeouts = 0;
  // 传输层已经判定为旧代际或重复终态、因此没有交给上层的输出事件数。它把“旧结果被丢弃”
  // 和“传输故障”分开记账：过滤不改变 last_error()，也不会停止子进程。
  std::uint64_t stale_output_events_filtered = 0;
  // 调用方在收到终态或取消超时后主动丢弃的待取输出事件数。它说明旧会话的队列不会跨越
  // 新的 start 生命周期继续增长；不代表 socket 中已被 ZeroMQ 接收的数据量。
  std::uint64_t discarded_output_events = 0;
  // 非阻塞停止申请次数。request_stop() 可被取消超时路径调用，真正回收仍由 stop() 完成。
  std::uint64_t requested_stops = 0;
  std::size_t input_queue_capacity = 0;
  std::size_t output_queue_capacity = 0;
  std::size_t input_queue_peak = 0;
  std::size_t output_queue_peak = 0;
  // 快照瞬间仍未被调用方取走的输出事件数。它用于验证取消后队列是否已经收敛到空；
  // 不能替代“发送端已经停止产生”或“socket 已排空”的结论。
  std::size_t output_queue_size = 0;
  std::size_t terminal_cache_capacity = 0;
  std::size_t terminal_cache_entries = 0;
  std::size_t terminal_cache_peak_entries = 0;
  std::size_t terminal_cache_expired_markers = 0;
};

class RemoteSessionProxy final {
 public:
  explicit RemoteSessionProxy(RemoteSessionProxyConfig config = {});
  ~RemoteSessionProxy();

  RemoteSessionProxy(const RemoteSessionProxy&) = delete;
  RemoteSessionProxy& operator=(const RemoteSessionProxy&) = delete;

  // 启动子进程、等待就绪、读取端点、完成数据面握手并启动 I/O 线程。成功返回后可以调用
  // queue_* 与 receive_events。重复 start（未 stop）返回 kAlreadyCompleted；start 失败时
  // 子进程与通道都已经回收，对象可以修正配置后重试（需要新对象重新校验）。
  domain::OperationResult start();

  // 停止代理：请求取消、等待 I/O 线程收敛、停止并回收子进程、关闭数据面通道。幂等。
  // 可在任意线程调用，但不得与 start 或 queue_* 并发进入同一个代理生命周期之外。
  domain::OperationResult stop() noexcept;

  // 请求停止但不等待回收：置停止标志、丢弃尚未发送的输入、关闭输出队列并请求子进程停止。
  // 供取消超时路径快速脱离卡死的后端使用；非阻塞、幂等。调用后仍必须用 stop() 或析构
  // 完成 I/O 线程 join 和子进程 waitpid；重复调用不会重复增加资源。
  void request_stop() noexcept;

  // 丢弃输出队列中尚未被 receive_events() 取走的事件，返回丢弃条数。它只清理旧的软件队列，
  // 不关闭通道、不停止子进程，也不改变 last_error()；下一轮 start 仍会重建独立队列。
  std::size_t discard_output_events() noexcept;

  // 输入生产接口。stream_id 非空、generation 只是流身份的一部分；同一时刻只允许一个
  // 未结束的输入流。所有 queue_* 可在不同线程并发调用，内部按调用顺序线性化。
  domain::OperationResult queue_start_stream(std::string stream_id,
                                             std::uint64_t generation = 0);
  domain::OperationResult queue_frame(const domain::AudioFrame& frame);
  // 结束输入流：valid_samples 为 0 表示整帧结束；1..319 时必须提供补零尾帧。
  domain::OperationResult queue_end_stream(std::size_t valid_samples = 0,
                                           std::optional<domain::AudioFrame> tail = std::nullopt);
  // 取消当前输入流。实现不把取消事件塞进有界输入队列，因此输入队列已满时也能受理；
  // I/O 线程会先发送此前已入队的输入事件，再发送取消事件。重复取消幂等成功；未开始或
  // 已经自然结束的流返回 kInvalidInput。
  domain::OperationResult queue_cancel_stream();

  // 取回已到达的输出事件，最多 max_events 条，追加到 out。超时返回 0；返回 0 不代表
  // 服务端已经结束，调用方应结合最后一条 end=true 事件或 stop() 判断。错误通过 last_error()
  // 报告；发生错误时不会阻塞。
  std::size_t receive_events(std::vector<protocol::DataEvent>& out,
                             std::size_t max_events,
                             std::chrono::milliseconds timeout);

  // 查询终态缓存。命中返回事件副本；已过期返回 kTimeout；从未记录或过期标记已被容量
  // 淘汰返回 kAlreadyCompleted。查询无阻塞、不改变缓存保留期限。
  domain::Result<protocol::DataEvent> query_terminal(const std::string& request_id,
                                                     std::uint64_t generation) const;

  // 最近一次传输/进程错误快照；成功时为 kNone。返回副本，可跨线程调用。
  domain::Error last_error() const;

  // 代理是否已经启动且尚未 stop。
  bool running() const noexcept;

  // 账目快照。
  RemoteSessionProxyStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::transport

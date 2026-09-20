// Linux profile 生命周期：管理一个独立 Session 子进程从启动就绪、输入/输出、取消到清理的
// 完整过程，并保证重复启动、停止和重启不会留下进程、端口或临时文件。
//
// 职责与适用范围
// --------------
// 本类位于部署 profile 层，只关心“一次 Linux 多进程会话的进程与数据面生命周期”。它复用
// 现有 RemoteSessionProxy 完成 fork/exec、就绪握手、PAIR 数据通道、输入上行和输出接收，
// 不重新实现 Session 编排，也不解析外部 NDJSON 或控制 RPC。因此调用方看到的是领域事件与
// 结构化错误，而不是 POSIX 句柄、socket 类型或 waitpid 细节。
//
// 拓扑与所有权
// ------------
// 父进程持有 LinuxProfile；子进程运行已经实现好的远端 Session 节点。LinuxProfile 创建并
// 拥有一个 RemoteSessionProxy，后者拥有子进程、数据通道和 I/O 线程。配置中的
// RemoteSessionProxyConfig::child 描述子进程可执行文件、环境和启动预算；调用方负责保证
// 端点文件路径不与其他并发 profile 冲突。析构或 stop() 会取消当前输入并回收子进程。
//
// 状态迁移不变量
// --------------
//   Idle --start()--> Starting --成功--> Ready --start_stream()--> Streaming
//   Starting --失败且回滚成功--> Idle
//   Ready/Streaming --stop()--> Stopping --清理成功--> Idle
//   Stopping --清理失败--> Unavailable（槽位不再复用，只能再尝试清理）
//   Unavailable --stop() 重试成功--> Idle
// 一次 start() 成功后只允许开始一条输入流；流进入终态或取消受理后必须 stop()/restart() 才能
// 运行下一轮，避免同一个子进程上复用旧输入游标、旧代际或旧输出水位。
//
// 取消与丢弃
// ----------
// cancel_stream() 只负责受理输入流取消，远端随后交付的终态仍会经过 poll_events() 交付。
// stop() 是关闭路径：它先封锁新的输入，再清空尚未取走的输出队列，然后由代理发出取消并
// 回收子进程。清空发生在停止之前，因此已经丢弃的旧输出不会在重启后重新出现；清空数量
// 由 RemoteSessionProxyStats::discarded_output_events 记录。终态交付后 poll_events() 只做
// 非阻塞排空并拒绝同一轮的迟到事件，不再把它们交给调用方。
//
// 线程与并发
// ----------
// 本类的状态字段没有内部锁；start()/stop()/restart()/输入接口/poll_events() 必须由同一个
// 调用线程串行驱动。RemoteSessionProxy 的 I/O 线程与本类状态无关，只负责 socket 收发。
// 和 Gateway 一样，这里把“谁拥有驱动线程”写成调用方契约，避免用锁掩盖一次 profile 被两个
// 线程同时启停的错误。start()/stop() 可能阻塞有限时间，上界由 RemoteSessionProxyConfig
// 中的启动/停止预算决定，绝不无限等待。
//
// 版本兼容
// --------
// 配置结构体字段只追加；枚举数值不参与线协议，但为了持久化证据可读，状态名保持稳定。
// 子进程就绪协议与数据面线格式沿用 RemoteSessionProxy 的版本约定，本类不另造一套。
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../domain/audio_frame.hpp"
#include "../domain/error.hpp"
#include "../protocol/data_event.hpp"
#include "../transport/remote_session_proxy.hpp"

namespace nexweave::app {

// Linux profile 的生命周期状态。数值不参与线协议，字符串名用于日志、证据与测试断言。
enum class LinuxProfileState : std::uint8_t {
  // 空闲：没有子进程，可以开始新的 profile。
  kIdle = 0,
  // 启动中：start() 已受理，正在等待子进程就绪与数据面握手；同一调用线程内可见。
  kStarting = 1,
  // 就绪：子进程已就绪，尚未开始输入流，可以 start_stream()。
  kReady = 2,
  // 流式：输入流已经开始，可以继续推送帧、结束流或取消流。
  kStreaming = 3,
  // 停止中：stop() 已受理，正在取消输入、丢弃输出并回收子进程。
  kStopping = 4,
  // 不可用：清理失败或状态无法安全重置；start()/restart() 拒绝继续，stop() 仍可重试。
  kUnavailable = 5,
};

const char* to_string(LinuxProfileState state) noexcept;

// Linux profile 配置。proxy 描述子进程、数据通道与容量；stream_id 和 first_generation 描述
// 本 profile 第一轮流身份。所有字段都在构造时校验，非法配置会在 start() 返回结构化
// kInvalidInput，而不是带着未知配置去 fork 子进程。
struct LinuxProfileConfig {
  // 子进程与数据面配置。必须通过 validate_remote_session_proxy_config()。
  transport::RemoteSessionProxyConfig proxy{};
  // 每次 start() 建立的输入流标识。必须非空；不同 profile 实例应使用不同值，便于把远端
  // 输出归属回本次运行。
  std::string stream_id = "linux-profile";
  // 第一条输入流的代际。必须为正；每次成功 start() 后自动递增，重启不会复用旧代际。
  std::uint64_t first_generation = 1;
};

// 配置校验：只读，不 fork、不创建 socket、不访问文件系统。proxy 配置、stream_id 和
// first_generation 必须有效；first_generation 不得取到使下一次自增回绕的保留值。
domain::OperationResult validate_linux_profile_config(const LinuxProfileConfig& config);

// 对外只读快照。它同时报告 profile 生命周期、当前流状态、自有账目和代理账目，便于在
// 证据里区分“进程还没起”“进程起了但输入没开始”“输入进行中”“清理完成”等事实。
struct LinuxProfileStatus {
  LinuxProfileState state = LinuxProfileState::kIdle;
  // 远端代理当前是否认为自己仍拥有一个已启动的子进程。
  bool process_running = false;
  // 是否已经开始过当前这一轮输入流。
  bool stream_started = false;
  // 当前轮输入是否已经通过 finish_stream() 结束。
  bool input_finished = false;
  // 当前轮是否已经受理取消；这不等于远端已经终态，也不等于进程已经退出。
  bool cancel_requested = false;
  // 是否已经向调用方交付过当前轮的终态事件。交付后不会再把同一轮迟到输出交给调用方。
  bool terminal_delivered = false;
  // 当前轮的代际；start_stream() 前为 0。
  std::uint64_t generation = 0;

  std::uint64_t start_attempts = 0;
  std::uint64_t starts_succeeded = 0;
  std::uint64_t start_failures = 0;
  std::uint64_t stop_requests = 0;
  std::uint64_t stops_succeeded = 0;
  std::uint64_t stop_failures = 0;
  std::uint64_t restarts_succeeded = 0;
  std::uint64_t streams_started = 0;
  std::uint64_t frames_queued = 0;
  std::uint64_t terminal_events = 0;
  // 终态之后被本层拒绝、没有交给调用方的迟到输出事件数。
  std::uint64_t late_events_filtered = 0;

  domain::Error last_start_error{};
  domain::Error last_error{};
  // 底层代理的队列容量、峰值、丢弃和停止账目；不含 socket 句柄。
  transport::RemoteSessionProxyStats proxy{};

  // 槽位是否可开始新的 profile。Unavailable 仍可通过 stop() 重试清理，但不允许直接 start()。
  bool slot_reusable() const noexcept {
    return state == LinuxProfileState::kIdle;
  }
};

// Linux 多进程会话 profile。
//
// 构造只校验配置并构造代理对象，不 fork、不绑定 socket、不创建 I/O 线程。start() 成功返回
// 后子进程已经完成就绪握手；stop() 是幂等的、有界的关闭入口；析构会调用一次 stop() 作为
// 最后兜底，但调用方仍应显式停止，以便读取清理结果。
class LinuxProfile final {
 public:
  explicit LinuxProfile(LinuxProfileConfig config = {});
  ~LinuxProfile();

  LinuxProfile(const LinuxProfile&) = delete;
  LinuxProfile& operator=(const LinuxProfile&) = delete;

  // 启动子进程并等待就绪。成功返回后状态为 kReady，尚不能推送音频；必须先 start_stream()。
  //
  // 失败语义：
  //   kInvalidInput   配置非法；没有副作用。
  //   kBusy           已经有 profile 在运行或正在停止；没有副作用。
  //   kBackendFailure 槽位已不可用，或子进程启动、就绪握手、端点读取失败。
  //   kTimeout        启动就绪超时；本方法已经回滚子进程与通道。
  //   kCancelled      启动期间收到停止或等价取消；本方法已经回滚。
  // 启动失败时本方法会再调用一次代理停止作为回滚兜底；若回滚也失败，状态进入 Unavailable，
  // 返回回滚错误，后续只能通过 stop() 重试清理，不能盲目重开。
  domain::OperationResult start();

  // 停止当前 profile。顺序固定为：封锁本层新工作 -> 清空尚未取走的输出 -> 通过代理取消
  // 输入并回收子进程。幂等；Idle 上调用只增加请求账目并成功返回。清理失败进入 Unavailable
  // 并保留错误，下一次 stop() 仍可重试；start() 在 Unavailable 上拒绝执行。
  domain::OperationResult stop() noexcept;

  // 先 stop() 再 start()。只有停止成功才启动新 profile；如果停止失败，保持 Unavailable，
  // 不会用一个可能仍握着旧资源的进程覆盖失败事实。
  domain::OperationResult restart();

  // 开始当前 profile 的输入流。必须先成功 start()，且每次 start() 只允许调用一次。
  // 输出后置：generation 与 stream_id 已随 start 事件上行；后续帧、结束或取消必须使用
  // 同一代际语义。失败返回结构化错误，状态留在 kReady，调用方可 stop() 回滚。
  domain::OperationResult start_stream();

  // 推送一帧 20 ms PCM。仅当输入流已经开始且尚未结束、取消或进入终态时有效。
  // 队列满时返回 kBackendFailure，并保留代理错误；调用方应停止或重启 profile，而不是
  // 用无界等待掩盖慢消费者。
  domain::OperationResult push_frame(const domain::AudioFrame& frame);

  // 结束输入流。valid_samples 与 tail 的规则由交互契约和代理定义：0 表示整帧结束，
  // 1..319 时 tail 必须携带补零后的尾帧。成功后不允许再推送帧。
  domain::OperationResult finish_stream(
      std::size_t valid_samples = 0,
      std::optional<domain::AudioFrame> tail = std::nullopt);

  // 受理当前输入流取消。它只表示取消请求已经交给代理，不表示远端已停止或终态已到达；
  // 远端终态仍通过 poll_events() 交付。重复调用幂等成功。
  domain::OperationResult cancel_stream();

  // 非阻塞优先地取回已到达的数据事件，最多 max_events 条，追加到 out。timeout 只在当前
  // 轮尚未交付终态时用于等待第一条事件；一旦当前轮终态已经交付，后续调用固定零等待排空
  // 并拒绝迟到事件，不会因为调用方传入较大 timeout 而阻塞在“已经没有合法输出”的流上。
  // 返回实际追加到 out 的事件数。未启动或已停止时返回 0。
  std::size_t poll_events(std::vector<protocol::DataEvent>& out,
                          std::size_t max_events,
                          std::chrono::milliseconds timeout =
                              std::chrono::milliseconds::zero());

  // 最近一次本层记录的错误；本层没有记录时回退到代理最近一次传输/进程错误。返回副本。
  domain::Error last_error() const noexcept;

  // 远端代理是否拥有已经启动的子进程。与状态机状态分开报告，便于识别“状态已拒绝新工作，
  // 但底层清理仍未完成”的窗口。
  bool running() const noexcept;

  // 一致只读快照。调用间不保证事务；调用方应在同一驱动线程上读取。
  LinuxProfileStatus status() const;

 private:
  LinuxProfileConfig config_;
  domain::Error config_error_{};
  transport::RemoteSessionProxy proxy_;

  LinuxProfileState state_ = LinuxProfileState::kIdle;
  bool stream_started_ = false;
  bool input_finished_ = false;
  bool cancel_requested_ = false;
  bool terminal_delivered_ = false;
  std::uint64_t generation_ = 0;
  std::uint64_t next_generation_ = 0;

  std::uint64_t start_attempts_ = 0;
  std::uint64_t starts_succeeded_ = 0;
  std::uint64_t start_failures_ = 0;
  std::uint64_t stop_requests_ = 0;
  std::uint64_t stops_succeeded_ = 0;
  std::uint64_t stop_failures_ = 0;
  std::uint64_t restarts_succeeded_ = 0;
  std::uint64_t streams_started_ = 0;
  std::uint64_t frames_queued_ = 0;
  std::uint64_t terminal_events_ = 0;
  std::uint64_t late_events_filtered_ = 0;

  domain::Error last_start_error_{};
  domain::Error last_error_{};
};

}  // namespace nexweave::app

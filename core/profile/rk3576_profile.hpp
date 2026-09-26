// RK3576 多进程部署 profile：按显式拓扑启动、等待就绪、停止并回滚一组协作进程。
//
// 职责与适用范围
// --------------
// 本类位于部署 profile 层，只负责“一组进程的角色、启动顺序、就绪边界和清理顺序”。它不解析
// 模型配置、不打开 ALSA、不传输音频，也不把 POSIX 句柄暴露给 Session。模型与设备能力由每个
// 子进程自己通过统一适配器接入；profile 只保证这些进程按拓扑出现和收敛。
//
// 拓扑与所有权
// ------------
// 调用方在 Rk3576ProfileConfig::processes 中声明每个进程的名字、子进程启动参数、依赖关系、
// 是否为音频设备唯一拥有者、是否通过音频适配器消费前端资源。profile 创建并拥有每个
// runtime::ChildProcess；子进程由 profile fork/exec、等待、发送信号和回收，调用方不持有 PID。
// 一个配置中必须且只能有一个 owns_audio_device 节点，它不能依赖其他节点；所有
// uses_audio_adapter 节点必须在传递依赖上排在拥有者之后，从拓扑层阻止两个进程争抢设备。
//
// 启动就绪顺序
// ------------
// start() 按依赖拓扑排序，先启动音频拥有者，再启动 ASR/LLM/TTS/Session 等适配器进程。
// 每个子进程必须完成自己的设备/SDK/端口准备后调用 runtime::notify_parent_ready()；本层等到
// 该显式事件才启动下一个节点，不使用 sleep 猜测就绪。任一下游启动失败时，已经就绪的节点
// 按相反顺序停止并回收；回滚本身失败则进入 Unavailable，拒绝盲目重开但允许 stop() 重试。
//
// 停止与异常
// ----------
// stop() 按启动顺序的反序停止，先阻断新业务再回收底层进程；每一跳都有 ChildProcess 的停止
// 预算和 SIGKILL 升级预算。子进程异常退出、停止预算耗尽或清理失败都由 ChildProcess 归档，
// profile 只汇总为结构化错误和逐进程状态，不把“信号已发”说成“进程已经收敛”。
//
// 线程与并发
// ----------
// 状态字段无内部锁；start()/stop()/restart()/status() 必须由同一拥有者线程串行调用。
// ChildProcess::status() 可在任意线程做只读快照，但本类公开接口不做跨线程并发驱动。
//
// 资源与版本
// ----------
// 本层创建的资源只有子进程和 ChildProcess 内部的就绪管道；不创建 socket、设备句柄或临时
// 文件。Rk3576ProcessSpec 字段只追加，枚举数值不进入线协议。子进程就绪协议沿用
// NEXWEAVE_READY_FD 私有约定，不在本层另造通知格式。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../domain/error.hpp"
#include "../runtime/child_process.hpp"

namespace nexweave::app {

// RK3576 部署生命周期状态。数值不参与线协议；字符串名用于日志、证据与测试断言。
enum class Rk3576ProfileState : std::uint8_t {
  // 空闲：没有活跃子进程，可以开始新的部署轮次。
  kIdle = 0,
  // 启动中：正在按拓扑顺序等待各子进程就绪；失败会走回滚。
  kStarting = 1,
  // 就绪：所有拓扑节点都已经显式报告就绪，可以接业务或等待上层关闭。
  kReady = 2,
  // 停止中：正在按反序发送停止请求并等待回收。
  kStopping = 3,
  // 不可用：回滚或停止清理失败，或状态无法安全重置；start() 拒绝继续，stop() 仍可重试。
  kUnavailable = 4,
};

const char* to_string(Rk3576ProfileState state) noexcept;

// 单个部署进程的声明。name 是本 profile 内的逻辑角色名，也是依赖引用的键。
struct Rk3576ProcessSpec {
  // 逻辑角色名，例如 "audio-frontend"、"asr"、"session"。必须非空且在配置内唯一。
  std::string name;
  // 子进程启动参数；executable 必须非空，参数/环境不得含内嵌 NUL。
  runtime::ChildProcessSpec process;
  // 子进程生命周期预算。每个节点独立配置启动、停止和强杀等待上限。
  runtime::ChildProcessConfig lifecycle;
  // 本节点必须先就绪的节点名列表。必须引用本配置内的其他节点，不能自引用或形成环。
  std::vector<std::string> start_after;
  // 是否拥有音频采集/播放资源。整个配置必须恰好一个为 true；拥有者不能依赖其他节点。
  bool owns_audio_device = false;
  // 是否通过音频适配器消费采集/播放资源。为 true 时必须在传递依赖上位于音频拥有者之后。
  bool uses_audio_adapter = false;
};

// RK3576 部署拓扑配置。空进程列表非法；所有约束在构造时静态校验，不会带着未知拓扑 fork。
struct Rk3576ProfileConfig {
  std::vector<Rk3576ProcessSpec> processes;
};

// 配置校验：只读，不 fork、不创建 socket、不访问文件系统。校验角色唯一性、依赖存在性、
// 无环、音频拥有者唯一且为根节点，以及所有适配器节点在拓扑上晚于音频拥有者。
domain::OperationResult validate_rk3576_profile_config(const Rk3576ProfileConfig& config);

// 单个进程的只读快照，用于证据里区分“未启动”“启动失败”“已就绪”“已退出”和“清理失败”。
struct Rk3576ProcessStatus {
  std::string name;
  bool owns_audio_device = false;
  bool uses_audio_adapter = false;
  // 本轮 start() 是否成功启动过该节点。
  bool started = false;
  // 本轮节点是否处于就绪运行态。
  bool ready = false;
  runtime::ChildProcessState process_state = runtime::ChildProcessState::kIdle;
  runtime::ChildProcessIdentity identity{};
  // 最近一次观察到的 PID，仅用于诊断；0 表示没有活跃或最近没有子进程。
  int native_pid = 0;
  // 最近一次启动错误；成功启动后由下一次 start 清空。
  domain::Error start_error{};
  // 最近一次退出归档；还没退出时 kind 为 kNone。
  runtime::ChildProcessExit last_exit{};
};

// 一致只读快照。调用方应在同一驱动线程上读取；跨调用不保证事务。
struct Rk3576ProfileStatus {
  Rk3576ProfileState state = Rk3576ProfileState::kIdle;
  // 是否所有声明节点都已经就绪并仍在运行。
  bool running = false;
  // 音频设备唯一拥有者的逻辑名；配置非法时为空。
  std::string audio_owner_name;
  // 音频拥有者本轮是否已就绪。
  bool audio_owner_ready = false;
  // 所有 uses_audio_adapter 节点本轮是否都已就绪；没有适配器节点时为 false。
  bool audio_adapter_ready = false;
  std::vector<Rk3576ProcessStatus> processes;

  std::uint64_t start_attempts = 0;
  std::uint64_t starts_succeeded = 0;
  std::uint64_t start_failures = 0;
  std::uint64_t stop_requests = 0;
  std::uint64_t stops_succeeded = 0;
  std::uint64_t stop_failures = 0;
  std::uint64_t restarts_succeeded = 0;

  domain::Error last_start_error{};
  domain::Error last_error{};

  // 槽位是否允许 start()。Unavailable 仍可用 stop() 重试清理，但不能直接重开。
  bool slot_reusable() const noexcept {
    return state == Rk3576ProfileState::kIdle;
  }
};

// RK3576 多进程部署对象。
//
// 构造只校验拓扑并构造每个 ChildProcess 槽位，不 fork、不创建管道。start() 成功返回后所有
// 节点都已报告就绪；stop() 幂等且有界；析构会调用一次 stop() 作为最后兜底。
class Rk3576Profile final {
 public:
  explicit Rk3576Profile(Rk3576ProfileConfig config = {});
  ~Rk3576Profile();

  Rk3576Profile(const Rk3576Profile&) = delete;
  Rk3576Profile& operator=(const Rk3576Profile&) = delete;

  // 按验证后的拓扑顺序启动所有节点。每个节点的 start() 都等待自己的显式就绪通知。
  //
  // 失败语义：
  //   kInvalidInput   配置非法；没有副作用。
  //   kBusy           当前已有轮次在运行或停止；没有副作用。
  //   kBackendFailure 任一子进程未就绪、提前退出或进程槽不可用；已启动节点会回滚。
  //   kTimeout        子进程在启动预算内未报告就绪；已启动节点会回滚。
  //   kCancelled      子进程启动期间收到取消；已启动节点会回滚。
  // 若回滚也失败，状态进入 Unavailable，并返回回滚错误；后续只能通过 stop() 重试清理。
  domain::OperationResult start();

  // 按启动顺序的反序停止所有节点。已有子进程会先收到 SIGTERM，并在各自预算内升级 SIGKILL。
  // 任一跳清理失败都会继续尝试其余节点，最后进入 Unavailable；下一次 stop() 仍可重试。
  domain::OperationResult stop() noexcept;

  // 先 stop() 再 start()。停止失败时不会覆盖失败事实。
  domain::OperationResult restart();

  // 是否处于启动中或已就绪；停止中与不可用都返回 false。
  bool running() const noexcept;

  // 最近一次 profile 自身记录的错误；没有则返回最后一个非空的进程启动错误。
  domain::Error last_error() const noexcept;

  // 逐进程和计数的只读快照。内部会对每个 ChildProcess 做一次非阻塞 waitpid 观察。
  Rk3576ProfileStatus status() const;

 private:
  struct Node;

  // 按反序停止已经启动或仍存活的节点。供私有 stop()/回滚路径共用。
  // 返回后：成功的节点已复位，失败的节点保留原始错误供上层进入 Unavailable。
  domain::OperationResult StopStartedNodes() noexcept;

  Rk3576ProfileConfig config_;
  domain::Error config_error_{};
  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<std::size_t> start_order_;
  std::size_t audio_owner_index_ = 0;
  bool audio_owner_configured_ = false;

  Rk3576ProfileState state_ = Rk3576ProfileState::kIdle;
  std::uint64_t start_attempts_ = 0;
  std::uint64_t starts_succeeded_ = 0;
  std::uint64_t start_failures_ = 0;
  std::uint64_t stop_requests_ = 0;
  std::uint64_t stops_succeeded_ = 0;
  std::uint64_t stop_failures_ = 0;
  std::uint64_t restarts_succeeded_ = 0;

  domain::Error last_start_error_{};
  domain::Error last_error_{};
};

}  // namespace nexweave::app

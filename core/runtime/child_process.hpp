// 子进程生命周期原语：在受控的父子进程边界上完成启动就绪、停止升级和退出归档。
//
// 职责与适用范围
// --------------
// 本类只管理**一个**操作系统子进程，不解释子进程承载的业务，也不把 POSIX 进程句柄或
// 就绪管道暴露给 Session。调用方提供可执行文件、参数、环境和就绪通知方式；本类负责
// fork/exec、等待就绪、发送信号、等待退出、回收资源，并把退出原因整理成稳定快照。
//
// 启动就绪
// --------
// start() 默认要求子进程通过 notify_parent_ready() 显式通知“可以接业务”。通知走父进程
// 创建的匿名管道，管道写端编号通过环境变量 NEXWEAVE_READY_FD 传给子进程；公共头文件只
// 暴露通知函数，不暴露文件描述符类型。只有收到通知（或调用方显式声明 expect_ready_signal
// 为 false）后 start() 才返回成功，因此“进程存在”与“业务就绪”是两个不同事实。
//
// 身份与迟到操作
// --------------
// 每次被受理的 start() 都会分配一个单调递增、永不回绕复用的 ChildProcessIdentity。停止、
// 等待和状态查询都必须携带期望身份；针对旧身份的操作返回 kAlreadyCompleted 且没有副作用。
// 身份不是 PID：PID 可能被操作系统复用，本类不提供“按 PID 找回旧进程”的入口。
//
// 停止与超时升级
// --------------
// request_stop() 只发送 SIGTERM，不等待；stop() 先发送 SIGTERM，在停止预算内等待退出，
// 超时后升级 SIGKILL，再在强杀预算内等待回收。销毁对象时也走同一条有界收敛路径。超过
// 强杀预算仍无法回收时对象进入不可用状态，不会假装槽位可以复用。
//
// 资源归属
// --------
// 本类创建并拥有：子进程、父进程侧的匿名管道、以及可选的日志文件重定向由子进程打开。
// 子进程由本类 fork/exec，由本类 waitpid 回收；调用方不直接持有 PID 或管道。子进程在
// exec 前只调用异步信号安全或本类明确说明的 POSIX 接口。
//
// 线程与并发
// ----------
// status() 与 request_stop() 可从任意线程调用；start()、stop()、wait_for_exit() 必须由
// 同一个拥有者线程串行调用，且不得与彼此并发。request_stop() 在内部只取常数时间的锁并
// 发送信号，不等待子进程收敛，因此可用于取消路径。
//
// 版本兼容
// --------
// 公共结构体字段可追加，既有字段的语义和错误码沿用领域错误契约，不重排枚举值。就绪环境
// 变量名属于父子进程之间的本版私有约定；变更它需要同时更新调用 notify_parent_ready() 的
// 子进程。
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../domain/error.hpp"

namespace nexweave::runtime {

// 子进程就绪端口的父进程侧环境变量名。子进程调用 notify_parent_ready() 时读取它。
inline constexpr char kChildReadyFdEnvironment[] = "NEXWEAVE_READY_FD";

// 子进程身份：每次成功受理 start() 后由父进程分配。0 表示无效身份。
//
// 身份只增不减、不回收复用；它不是 PID。进程被重建后，旧身份不会被赋给新进程，因此旧连接
// 或旧控制请求不能借助“PID 恰好对应上”恢复成对新进程的操作。
struct ChildProcessIdentity {
  std::uint64_t value = 0;

  bool valid() const noexcept {
    return value != 0;
  }
};

bool operator==(ChildProcessIdentity lhs, ChildProcessIdentity rhs) noexcept;
bool operator!=(ChildProcessIdentity lhs, ChildProcessIdentity rhs) noexcept;

// 单个子进程槽位的对外状态。数值不参与线协议，新增状态只追加。
enum class ChildProcessState : std::uint8_t {
  // 空闲：没有子进程，也没有未回收的僵尸；可以受理下一次 start()。
  kIdle = 0,
  // 启动中：fork/exec 已受理，正在等待就绪通知；槽位不可复用。
  kStarting = 1,
  // 运行中：子进程已经就绪，可以接业务。
  kRunning = 2,
  // 停止中：停止已受理，子进程还没被 waitpid 回收；槽位不可复用。
  kStopping = 3,
  // 不可用：强杀预算内仍未回收，或发生了无法安全重置的等待错误；此后不再受理 start()。
  kUnavailable = 4
};

const char* to_string(ChildProcessState state) noexcept;

// 子进程退出原因分类。它回答“是怎样结束的”，不替代原始 exit code / signal number。
enum class ChildProcessExitKind : std::uint8_t {
  // 还没有退出记录。
  kNone = 0,
  // waitpid 观察到正常退出（WIFEXITED），且没有受理过停止请求。
  kExited = 1,
  // waitpid 观察到信号终止（WIFSIGNALED），且没有受理过停止请求；通常表示崩溃或外部强杀。
  kSignaled = 2,
  // 停止已受理后子进程结束；可能是它自行处理 SIGTERM 后退出，也可能是收到终止信号。
  kStopped = 3,
  // 停止等待超时后由本类发送 SIGKILL 并回收。
  kKilled = 4
};

const char* to_string(ChildProcessExitKind kind) noexcept;

// 一次退出归档。字段全部是 waitpid 状态和本类停止意图的派生结果，不拥有任何资源。
struct ChildProcessExit {
  ChildProcessExitKind kind = ChildProcessExitKind::kNone;
  // WIFEXITED 时有效，否则为 -1。
  int exit_code = -1;
  // WIFSIGNALED 时有效（如 SIGTERM、SIGKILL、SIGABRT），否则为 0。
  int signal_number = 0;
  // 本次退出前是否已经通过 request_stop()/stop() 受理过停止。
  bool stop_requested = false;
  // 是否由本类在停止预算用尽后升级为 SIGKILL。
  bool forced = false;
  // 归档时附带的诊断错误；正常退出和主动停止都保持 ok()。
  domain::Error error{};

  bool ok() const noexcept {
    return error.ok();
  }
};

// 子进程生命周期配置。所有等待都有显式预算；非法值在构造时整份回退到默认配置，调用方也可
// 先用 validate_child_process_config() 单独校验。
struct ChildProcessConfig {
  // 等待子进程发出就绪通知的预算。超时会触发停止升级并回滚本次启动。
  std::chrono::milliseconds start_wait_budget{5000};
  // stop() 在发送 SIGTERM 后等待正常退出的预算。必须为正。
  std::chrono::milliseconds stop_wait_budget{5000};
  // 升级 SIGKILL 后等待回收的预算。必须为正。
  std::chrono::milliseconds kill_wait_budget{1000};
  // start()/wait_for_exit() 轮询子进程状态与就绪管道的间隔；必须有界且为正。
  std::chrono::milliseconds poll_interval{10};
  // true 表示停止时向子进程进程组发送信号；false 只向子进程 PID 发送信号。
  // fork 后本类会先尝试把子进程放入独立进程组；设置失败时仍回退到 PID 信号。
  bool kill_process_group = true;
};

// 配置校验：只读、不 fork、不创建管道。预算或轮询间隔非正返回 kInvalidInput。
domain::OperationResult validate_child_process_config(const ChildProcessConfig& config);

// 子进程启动参数。executable 必须非空；参数和环境中的字符串不得包含内嵌 NUL。
// environment 是 "KEY=VALUE" 形式的覆盖项；inherit_environment 为真时在父进程环境副本上
// 覆盖，为假时只使用 environment 项与就绪端口变量。
struct ChildProcessSpec {
  // 可执行文件路径。含 '/' 时按其语义解析；不含 '/' 时不隐式搜索 PATH，以避免父进程
  // 与子进程对同一 PATH 的解析出现分歧。
  std::string executable;
  // argv[1..]；argv[0] 由本类填成 executable。
  std::vector<std::string> arguments;
  // 是否继承父进程环境。
  bool inherit_environment = true;
  // "KEY=VALUE" 覆盖项；键不得为空。
  std::vector<std::string> environment;
  // 非空时在 exec 前 chdir；空串表示继承父进程工作目录。
  std::string working_directory;
  // 非空时把子进程 stdout/stderr 覆盖到该文件（O_TRUNC）。空串表示继承父进程。
  std::string stdout_path;
  std::string stderr_path;
  // true：start() 必须等到子进程调用 notify_parent_ready()。false：exec 成功后立即返回，
  // 只适用于调用方已经用外部探针确认就绪的场景；它会削弱“启动就绪”保证。
  bool expect_ready_signal = true;
};

domain::OperationResult validate_child_process_spec(const ChildProcessSpec& spec);

// 子进程状态快照。所有字段都是锁内一致读取的副本，不暴露内部句柄。
struct ChildProcessStatus {
  ChildProcessState state = ChildProcessState::kIdle;
  // 当前活跃（启动中/运行中/停止中）的身份；空闲时为 0。
  ChildProcessIdentity identity{};
  // 最近一次被受理的启动身份；即使该进程已经退出也保留，用于判断迟到操作是否过期。
  ChildProcessIdentity last_identity{};
  // 当前是否有尚未 waitpid 回收的子进程。
  bool process_alive = false;
  // 当前活跃子进程是否已经受理停止。
  bool stop_requested = false;
  // 最近一次观察到的 PID。仅供诊断和故障注入，不能当身份使用。
  int native_pid = 0;
  // 账目：start_attempts 是受理过的启动次数；ready_starts 是真正走到运行态的会话数。
  std::uint64_t start_attempts = 0;
  std::uint64_t ready_starts = 0;
  // 正常退出、崩溃与强杀的分类计数；一次退出只进入其中一类。
  std::uint64_t clean_exits = 0;
  std::uint64_t crash_exits = 0;
  std::uint64_t forced_kills = 0;
  // 最近一次启动失败原因；成功启动后复位。
  domain::Error last_start_error{};
  // 最近一次退出归档。进程还没有退出时 kind 为 kNone。
  ChildProcessExit last_exit{};

  bool slot_reusable() const noexcept {
    return state == ChildProcessState::kIdle;
  }
};

// 子进程侧就绪通知。应在所有输入/设备/监听资源准备完毕后调用一次；它只写一个字节到父进程
// 通过 NEXWEAVE_READY_FD 传入的管道。
//
// 错误语义：环境变量缺失或格式非法返回 kInvalidInput；写入失败返回 kDeviceFailure。
// 本函数不关闭、不复制、不延长文件描述符生命周期；文件描述符由父进程创建，子进程只写一次。
domain::OperationResult notify_parent_ready();

// 单槽位子进程生命周期对象。
//
// 构造只保存配置并做整份回退，不 fork、不创建管道；析构会尝试停止并回收仍然活跃的子进程。
// 对象不可复制；内部实现通过 pimpl 隐藏 POSIX 句柄，公共头文件不出现 pid_t、管道类型或
// waitpid 细节。
class ChildProcess final {
 public:
  explicit ChildProcess(ChildProcessConfig config = {});
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  // 受理一次启动并等待就绪。返回的成功身份是后续 stop()/wait_for_exit() 的唯一校验依据。
  //
  // 失败语义：
  //   kInvalidInput      spec 非法；没有副作用。
  //   kBusy              槽位已有子进程（启动中/运行中/停止中）；没有副作用。
  //   kBackendFailure    fork/exec 失败、子进程在就绪前退出，或槽位此前已不可用。
  //   kTimeout           在 start_wait_budget 内没有收到就绪通知；本类会停止并回收子进程。
  //   kCancelled         启动过程中收到停止请求；本类会停止并回收子进程。
  // 启动失败一律回滚本次创建的管道与子进程；槽位是否可复用由返回错误和 status() 共同回答。
  domain::Result<ChildProcessIdentity> start(const ChildProcessSpec& spec);

  // 非阻塞请求停止当前子进程。expected 为 0 表示当前活跃身份；非 0 时必须与当前身份一致，
  // 否则返回 false 且没有副作用。返回 true 只表示停止请求已受理，不表示子进程已经退出。
  //
  // 线程安全：可从任意线程调用。实现只取常数时间的互斥量并发送 SIGTERM，不 waitpid。
  bool request_stop(ChildProcessIdentity expected = {}) noexcept;

  // 停止当前子进程并等待回收。默认预算来自 stop_wait_budget / kill_wait_budget。
  // expected 为 0 表示当前活跃身份；已经按同一身份完成过停止时幂等返回上次退出归档。
  // 预算用尽但仍有回收可能时返回 kTimeout，槽位保持 kStopping 或转为 kUnavailable。
  domain::Result<ChildProcessExit> stop(
      ChildProcessIdentity expected = {},
      std::optional<std::chrono::milliseconds> stop_wait_budget = std::nullopt);

  // 等待当前子进程自然退出，不发送信号。expected 校验规则同 stop()。
  // budget 为空表示一直等到退出；为 0 表示只做一次非阻塞快照。
  // 已经回收过同一身份时返回上次退出归档；身份过期返回 kAlreadyCompleted。
  domain::Result<ChildProcessExit> wait_for_exit(
      ChildProcessIdentity expected = {},
      std::optional<std::chrono::milliseconds> wait_budget = std::nullopt);

  // 状态查询：锁内完成一次非阻塞 waitpid 观察后返回一致快照，不主动发送信号。
  ChildProcessStatus status() const;

  // 当前活跃身份是否等于 identity。已退出身份不再匹配，避免调用方用旧身份操作新进程。
  bool matches(ChildProcessIdentity identity) const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::runtime

#include "child_process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

extern char** environ;

namespace nexweave::runtime {

namespace {

// 父进程侧的 RAII 文件描述符。子进程侧不使用它，因为 fork 后不应依赖可能触发非异步信号
// 安全代码的包装类型；父进程和子进程分别显式 close 自己不需要的描述符。
class ScopedFd {
 public:
  ScopedFd() = default;
  explicit ScopedFd(int fd) : fd_(fd) {}

  ~ScopedFd() {
    Reset();
  }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  int Get() const noexcept {
    return fd_;
  }

  bool Valid() const noexcept {
    return fd_ >= 0;
  }

  void Reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

bool HasEmbeddedNul(const std::string& value) noexcept {
  return value.find('\0') != std::string::npos;
}

domain::Error MakeError(domain::ErrorCode code, std::string message) {
  return domain::OperationResult::failure(code, std::move(message)).error;
}

std::string ErrnoMessage(const char* action) {
  const int saved_errno = errno;
  return std::string(action) + ": " + std::strerror(saved_errno);
}

// 设置/覆盖 "KEY=VALUE" 环境项。调用方已保证 key 非空且不含 NUL。
void SetEnvironmentValue(std::vector<std::string>* environment, const std::string& key,
                         const std::string& value) {
  const std::string prefix = key + "=";
  for (std::string& entry : *environment) {
    if (entry.compare(0, prefix.size(), prefix) == 0) {
      entry = prefix + value;
      return;
    }
  }
  environment->push_back(prefix + value);
}

// 在父进程和子进程之间传递一个 errno；它只用于 exec/chdir/重定向失败，成功 exec 时
// CLOEXEC 让读端看到 EOF。
bool WriteErrnoToPipe(int fd, int error_number) noexcept {
  const ssize_t written = ::write(fd, &error_number, sizeof(error_number));
  return written == static_cast<ssize_t>(sizeof(error_number));
}

// 在父进程中读取子进程 exec 前写出的 errno。返回 0 表示成功的 exec 已经关闭了写端。
int ReadErrnoFromPipe(int fd) noexcept {
  int error_number = 0;
  ssize_t bytes = 0;
  do {
    bytes = ::read(fd, &error_number, sizeof(error_number));
  } while (bytes < 0 && errno == EINTR);
  if (bytes == static_cast<ssize_t>(sizeof(error_number))) {
    return error_number;
  }
  return 0;
}

// 发送信号。优先杀整个进程组；进程组不存在或尚未建立时回退到 PID，避免把未成组的子进程
// 留在无人回收的状态。
bool SendSignal(pid_t pid, int signal_number, bool kill_process_group) noexcept {
  if (pid <= 0) {
    return false;
  }
  if (kill_process_group && ::kill(-pid, signal_number) == 0) {
    return true;
  }
  return ::kill(pid, signal_number) == 0;
}

ChildProcessExit ClassifyWaitStatus(int status, bool stop_requested, bool forced_kill) {
  ChildProcessExit exit;
  exit.stop_requested = stop_requested;
  exit.forced = forced_kill;
  if (WIFEXITED(status)) {
    exit.kind = stop_requested ? ChildProcessExitKind::kStopped : ChildProcessExitKind::kExited;
    exit.exit_code = WEXITSTATUS(status);
    return exit;
  }
  if (WIFSIGNALED(status)) {
    exit.signal_number = WTERMSIG(status);
    if (forced_kill) {
      exit.kind = ChildProcessExitKind::kKilled;
    } else if (stop_requested) {
      exit.kind = ChildProcessExitKind::kStopped;
    } else {
      exit.kind = ChildProcessExitKind::kSignaled;
    }
    return exit;
  }
  exit.kind = ChildProcessExitKind::kNone;
  exit.error = MakeError(domain::ErrorCode::kBackendFailure,
                         "waitpid 返回了无法归档的等待状态");
  return exit;
}

std::chrono::milliseconds ClampBudget(std::chrono::milliseconds budget) {
  if (budget < std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  return budget;
}

}  // namespace

bool operator==(ChildProcessIdentity lhs, ChildProcessIdentity rhs) noexcept {
  return lhs.value == rhs.value;
}

bool operator!=(ChildProcessIdentity lhs, ChildProcessIdentity rhs) noexcept {
  return !(lhs == rhs);
}

const char* to_string(ChildProcessState state) noexcept {
  switch (state) {
    case ChildProcessState::kIdle:
      return "idle";
    case ChildProcessState::kStarting:
      return "starting";
    case ChildProcessState::kRunning:
      return "running";
    case ChildProcessState::kStopping:
      return "stopping";
    case ChildProcessState::kUnavailable:
      return "unavailable";
  }
  return "";
}

const char* to_string(ChildProcessExitKind kind) noexcept {
  switch (kind) {
    case ChildProcessExitKind::kNone:
      return "none";
    case ChildProcessExitKind::kExited:
      return "exited";
    case ChildProcessExitKind::kSignaled:
      return "signaled";
    case ChildProcessExitKind::kStopped:
      return "stopped";
    case ChildProcessExitKind::kKilled:
      return "killed";
  }
  return "";
}

domain::OperationResult validate_child_process_config(const ChildProcessConfig& config) {
  if (config.start_wait_budget <= std::chrono::milliseconds::zero()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "start_wait_budget 必须为正");
  }
  if (config.stop_wait_budget <= std::chrono::milliseconds::zero()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "stop_wait_budget 必须为正");
  }
  if (config.kill_wait_budget <= std::chrono::milliseconds::zero()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "kill_wait_budget 必须为正");
  }
  if (config.poll_interval <= std::chrono::milliseconds::zero()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "poll_interval 必须为正");
  }
  return domain::OperationResult::success();
}

domain::OperationResult validate_child_process_spec(const ChildProcessSpec& spec) {
  if (spec.executable.empty()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "executable 不能为空");
  }
  const std::string* strings[] = {&spec.executable, &spec.working_directory,
                                  &spec.stdout_path, &spec.stderr_path};
  for (const std::string* value : strings) {
    if (HasEmbeddedNul(*value)) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "进程参数不能包含内嵌 NUL");
    }
  }
  for (const std::string& argument : spec.arguments) {
    if (HasEmbeddedNul(argument)) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "命令行参数不能包含内嵌 NUL");
    }
  }
  for (const std::string& entry : spec.environment) {
    if (HasEmbeddedNul(entry)) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "环境项不能包含内嵌 NUL");
    }
    const std::size_t equal = entry.find('=');
    if (equal == std::string::npos || equal == 0) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "环境项必须为非空 KEY=VALUE");
    }
  }
  return domain::OperationResult::success();
}

domain::OperationResult notify_parent_ready() {
  const char* raw_fd = std::getenv(kChildReadyFdEnvironment);
  if (raw_fd == nullptr || *raw_fd == '\0') {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "缺少 NEXWEAVE_READY_FD 环境变量");
  }
  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(raw_fd, &end, 10);
  if (errno != 0 || end == raw_fd || *end != '\0' || parsed < 3 ||
      parsed > std::numeric_limits<int>::max()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "NEXWEAVE_READY_FD 不是有效的文件描述符");
  }
  const char byte = '1';
  ssize_t written = 0;
  do {
    written = ::write(static_cast<int>(parsed), &byte, 1);
  } while (written < 0 && errno == EINTR);
  if (written == 1) {
    return domain::OperationResult::success();
  }
  return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                          "向父进程写入就绪通知失败");
}

// 子进程槽位实现。全部字段与 POSIX 句柄都由同一把互斥量保护；等待循环在阻塞点前主动
// 解锁，使 request_stop()/status() 可以从其他线程进入。
struct ChildProcess::Impl {
  explicit Impl(ChildProcessConfig config) : config(config) {
    if (!validate_child_process_config(config).ok()) {
      this->config = ChildProcessConfig{};
    }
  }

  ~Impl() {
    Shutdown();
  }

  mutable std::mutex mutex;
  ChildProcessConfig config;
  ChildProcessState state = ChildProcessState::kIdle;
  ChildProcessIdentity active_identity{};
  ChildProcessIdentity last_identity{};
  std::uint64_t next_identity = 0;
  pid_t pid = -1;
  pid_t last_pid = -1;
  bool stop_requested = false;
  bool sigterm_sent = false;
  bool forced_kill = false;
  ChildProcessExit last_exit{};
  domain::Error last_start_error{};

  std::uint64_t start_attempts = 0;
  std::uint64_t ready_starts = 0;
  std::uint64_t clean_exits = 0;
  std::uint64_t crash_exits = 0;
  std::uint64_t forced_kills = 0;

  bool IdentityKnownLocked(ChildProcessIdentity expected) const {
    const ChildProcessIdentity target = expected.valid() ? expected : active_identity;
    if (!target.valid()) {
      return false;
    }
    return active_identity == target || last_identity == target;
  }

  bool ReapNonBlockingLocked() {
    if (pid <= 0) {
      return true;
    }
    int status = 0;
    const pid_t waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      FinalizeExitLocked(status, forced_kill);
      return true;
    }
    if (waited < 0 && errno == ECHILD) {
      // 子进程已被其他 waitpid 回收；本类无法再给出真实退出原因，因此保守地留下错误
      // 归档，并让槽位回到空闲，避免僵尸无法处理。
      const pid_t observed_pid = pid;
      pid = -1;
      last_pid = observed_pid;
      active_identity = ChildProcessIdentity{};
      state = ChildProcessState::kIdle;
      stop_requested = false;
      sigterm_sent = false;
      forced_kill = false;
      last_exit = ChildProcessExit{};
      last_exit.error = MakeError(domain::ErrorCode::kBackendFailure,
                                  "子进程句柄已被其他 waitpid 回收，退出原因不可知");
      return true;
    }
    return false;
  }

  void FinalizeExitLocked(int status, bool forced) {
    const pid_t observed_pid = pid;
    last_exit = ClassifyWaitStatus(status, stop_requested, forced);
    if (last_exit.kind == ChildProcessExitKind::kExited && last_exit.exit_code == 0) {
      clean_exits += 1;
    } else if (last_exit.kind == ChildProcessExitKind::kSignaled) {
      crash_exits += 1;
    }
    if (last_exit.forced) {
      forced_kills += 1;
    }
    pid = -1;
    last_pid = observed_pid;
    active_identity = ChildProcessIdentity{};
    state = ChildProcessState::kIdle;
    stop_requested = false;
    sigterm_sent = false;
    forced_kill = false;
  }

  domain::Result<ChildProcessIdentity> Start(const ChildProcessSpec& spec);
  bool RequestStop(ChildProcessIdentity expected) noexcept;
  domain::Result<ChildProcessExit> Stop(ChildProcessIdentity expected,
                                        std::optional<std::chrono::milliseconds> budget);
  domain::Result<ChildProcessExit> WaitForExit(
      ChildProcessIdentity expected,
      std::optional<std::chrono::milliseconds> budget);
  ChildProcessStatus Snapshot();
  bool Matches(ChildProcessIdentity identity) const noexcept;
  void Shutdown();
};

domain::Result<ChildProcessIdentity> ChildProcess::Impl::Start(const ChildProcessSpec& spec) {
  const domain::OperationResult valid_spec = validate_child_process_spec(spec);
  if (!valid_spec.ok()) {
    return domain::Result<ChildProcessIdentity>::failure(valid_spec.error.code,
                                                         valid_spec.error.message);
  }

  ChildProcessIdentity identity;
  {
    const std::lock_guard<std::mutex> guard(mutex);
    if (state == ChildProcessState::kUnavailable) {
      return domain::Result<ChildProcessIdentity>::failure(
          domain::ErrorCode::kBackendFailure, "槽位此前未能完成回收，不再复用");
    }
    if (state != ChildProcessState::kIdle) {
      return domain::Result<ChildProcessIdentity>::failure(
          domain::ErrorCode::kBusy, "已有子进程占用槽位，本次启动未被受理");
    }
    if (next_identity == std::numeric_limits<std::uint64_t>::max()) {
      return domain::Result<ChildProcessIdentity>::failure(
          domain::ErrorCode::kBackendFailure, "子进程身份已达到上限，无法在不回绕的前提下启动");
    }
    next_identity += 1;
    identity.value = next_identity;
    active_identity = identity;
    last_identity = identity;
    state = ChildProcessState::kStarting;
    stop_requested = false;
    sigterm_sent = false;
    forced_kill = false;
    last_exit = ChildProcessExit{};
    last_start_error = domain::Error{};
    start_attempts += 1;
  }

  // 先准备所有 exec 需要的字符串和描述符。除 fork 本身外，任何失败都在这里以普通错误
  // 返回，槽位会立即恢复为空闲，不会留下未回收的子进程。
  std::vector<std::string> argument_storage;
  argument_storage.reserve(spec.arguments.size() + 1);
  argument_storage.push_back(spec.executable);
  argument_storage.insert(argument_storage.end(), spec.arguments.begin(), spec.arguments.end());
  std::vector<char*> argv;
  argv.reserve(argument_storage.size() + 1);
  for (std::string& argument : argument_storage) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  std::vector<std::string> environment_storage;
  if (spec.inherit_environment) {
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
      environment_storage.emplace_back(*entry);
    }
  }
  for (const std::string& entry : spec.environment) {
    const std::size_t equal = entry.find('=');
    SetEnvironmentValue(&environment_storage, entry.substr(0, equal), entry.substr(equal + 1));
  }

  ScopedFd exec_error_read;
  ScopedFd exec_error_write;
  ScopedFd ready_read;
  ScopedFd ready_write;

  int exec_pipe[2] = {-1, -1};
  if (::pipe(exec_pipe) != 0) {
    const domain::Error error = MakeError(domain::ErrorCode::kBackendFailure,
                                          ErrnoMessage("创建 exec 错误管道失败"));
    const std::lock_guard<std::mutex> guard(mutex);
    active_identity = ChildProcessIdentity{};
    state = ChildProcessState::kIdle;
    last_start_error = error;
    return domain::Result<ChildProcessIdentity>::failure(error.code, error.message);
  }
  exec_error_read.Reset(exec_pipe[0]);
  exec_error_write.Reset(exec_pipe[1]);
  (void)::fcntl(exec_error_read.Get(), F_SETFD, FD_CLOEXEC);
  (void)::fcntl(exec_error_write.Get(), F_SETFD, FD_CLOEXEC);

  if (spec.expect_ready_signal) {
    int ready_pipe[2] = {-1, -1};
    if (::pipe(ready_pipe) != 0) {
      const domain::Error error = MakeError(domain::ErrorCode::kBackendFailure,
                                            ErrnoMessage("创建就绪管道失败"));
      const std::lock_guard<std::mutex> guard(mutex);
      active_identity = ChildProcessIdentity{};
      state = ChildProcessState::kIdle;
      last_start_error = error;
      return domain::Result<ChildProcessIdentity>::failure(error.code, error.message);
    }
    ready_read.Reset(ready_pipe[0]);
    ready_write.Reset(ready_pipe[1]);
    (void)::fcntl(ready_read.Get(), F_SETFD, FD_CLOEXEC);
    SetEnvironmentValue(&environment_storage, kChildReadyFdEnvironment,
                        std::to_string(ready_write.Get()));
  }

  std::vector<char*> envp;
  envp.reserve(environment_storage.size() + 1);
  for (std::string& entry : environment_storage) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) {
    const domain::Error error = MakeError(domain::ErrorCode::kBackendFailure,
                                          ErrnoMessage("fork 子进程失败"));
    const std::lock_guard<std::mutex> guard(mutex);
    active_identity = ChildProcessIdentity{};
    state = ChildProcessState::kIdle;
    last_start_error = error;
    return domain::Result<ChildProcessIdentity>::failure(error.code, error.message);
  }

  if (child == 0) {
    // 子进程路径只调用异步信号安全或本函数明确依赖的 POSIX 接口。所有字符串和指针都已在
    // fork 前准备好，避免在 fork 后分配内存或取得可能导致死锁的锁。
    (void)::setpgid(0, 0);
    if (!spec.working_directory.empty() && ::chdir(spec.working_directory.c_str()) != 0) {
      WriteErrnoToPipe(exec_error_write.Get(), errno);
      ::_exit(127);
    }
    if (!spec.stdout_path.empty()) {
      const int fd = ::open(spec.stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0) {
        WriteErrnoToPipe(exec_error_write.Get(), errno);
        ::_exit(127);
      }
      if (fd != STDOUT_FILENO) {
        ::close(fd);
      }
    }
    if (!spec.stderr_path.empty()) {
      const int fd = ::open(spec.stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0 || ::dup2(fd, STDERR_FILENO) < 0) {
        WriteErrnoToPipe(exec_error_write.Get(), errno);
        ::_exit(127);
      }
      if (fd != STDERR_FILENO) {
        ::close(fd);
      }
    }
    ::execve(spec.executable.c_str(), argv.data(), envp.data());
    WriteErrnoToPipe(exec_error_write.Get(), errno);
    ::_exit(127);
  }

  // 父进程不再需要子进程侧的描述符。先保存父进程侧读端，再关闭子进程侧写端。
  exec_error_write.Reset();
  ready_write.Reset();
  (void)::setpgid(child, child);
  {
    const std::lock_guard<std::mutex> guard(mutex);
    pid = child;
    last_pid = child;
    if (stop_requested && !sigterm_sent) {
      (void)SendSignal(pid, SIGTERM, config.kill_process_group);
      sigterm_sent = true;
    }
  }

  const auto start_deadline = std::chrono::steady_clock::now() + config.start_wait_budget;
  bool ready = false;
  while (true) {
    {
      const std::lock_guard<std::mutex> guard(mutex);
      if (stop_requested) {
        // 停止请求已受理；不在启动循环里继续等待就绪，交给统一的 Stop() 收敛。
        // 这里不持锁调用阻塞接口，先解锁后退出循环。
      } else if (ReapNonBlockingLocked()) {
        // 子进程在就绪前退出。ReapNonBlockingLocked 已经写入 last_exit 并把槽位置为空闲。
        const ChildProcessExit exit = last_exit;
        const std::string detail =
            std::string("子进程在就绪前退出: kind=") + to_string(exit.kind) +
            " code=" + std::to_string(exit.exit_code) + " signal=" +
            std::to_string(exit.signal_number);
        last_start_error = MakeError(domain::ErrorCode::kBackendFailure, detail);
        return domain::Result<ChildProcessIdentity>::failure(
            domain::ErrorCode::kBackendFailure, detail);
      }
    }

    bool must_stop = false;
    {
      const std::lock_guard<std::mutex> guard(mutex);
      must_stop = stop_requested;
    }
    if (must_stop) {
      const domain::Result<ChildProcessExit> stopped =
          Stop(identity, config.stop_wait_budget);
      if (!stopped.ok()) {
        const std::lock_guard<std::mutex> guard(mutex);
        last_start_error = stopped.error;
        return domain::Result<ChildProcessIdentity>::failure(
            stopped.error.code, stopped.error.message);
      }
      {
        const std::lock_guard<std::mutex> guard(mutex);
        const domain::Error error =
            MakeError(domain::ErrorCode::kCancelled, "子进程启动期间收到停止请求");
        last_start_error = error;
        return domain::Result<ChildProcessIdentity>::failure(error.code, error.message);
      }
    }

    struct pollfd descriptors[2];
    int descriptor_count = 0;
    if (exec_error_read.Valid()) {
      descriptors[descriptor_count].fd = exec_error_read.Get();
      descriptors[descriptor_count].events = POLLIN | POLLHUP;
      descriptors[descriptor_count].revents = 0;
      descriptor_count += 1;
    }
    if (ready_read.Valid()) {
      descriptors[descriptor_count].fd = ready_read.Get();
      descriptors[descriptor_count].events = POLLIN | POLLHUP;
      descriptors[descriptor_count].revents = 0;
      descriptor_count += 1;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= start_deadline) {
      break;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(start_deadline - now);
    const int timeout_ms = static_cast<int>(
        std::min<std::int64_t>(remaining.count(), config.poll_interval.count()));
    const int poll_result = ::poll(descriptors, descriptor_count, std::max(timeout_ms, 1));
    if (poll_result < 0 && errno != EINTR) {
      const domain::Error error = MakeError(domain::ErrorCode::kBackendFailure,
                                            ErrnoMessage("轮询子进程就绪状态失败"));
      // 轮询失败时子进程可能仍在运行。先走统一停止路径回滚，避免把进程与管道留在
      // “启动中”状态；如果停止也无法收敛，返回停止错误而不是掩盖资源状态。
      const domain::Result<ChildProcessExit> stopped = Stop(identity, config.stop_wait_budget);
      if (!stopped.ok()) {
        const std::lock_guard<std::mutex> guard(mutex);
        last_start_error = stopped.error;
        return domain::Result<ChildProcessIdentity>::failure(stopped.error.code,
                                                             stopped.error.message);
      }
      const std::lock_guard<std::mutex> guard(mutex);
      last_start_error = error;
      return domain::Result<ChildProcessIdentity>::failure(error.code, error.message);
    }
    if (poll_result <= 0) {
      continue;
    }

    for (int index = 0; index < descriptor_count; ++index) {
      const short revents = descriptors[index].revents;
      if ((revents & (POLLIN | POLLHUP)) == 0) {
        continue;
      }
      const int fd = descriptors[index].fd;
      if (fd == exec_error_read.Get()) {
        const int child_errno = ReadErrnoFromPipe(fd);
        if (child_errno != 0) {
          int status = 0;
          while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
          }
          const domain::Error error = MakeError(
              domain::ErrorCode::kBackendFailure,
              std::string("子进程 exec 失败: ") + std::strerror(child_errno));
          const std::lock_guard<std::mutex> guard(mutex);
          if (pid == child) {
            pid = -1;
            last_pid = child;
            active_identity = ChildProcessIdentity{};
            state = ChildProcessState::kIdle;
          }
          last_start_error = error;
          return domain::Result<ChildProcessIdentity>::failure(error.code, error.message);
        }
        exec_error_read.Reset();
        if (!spec.expect_ready_signal) {
          ready = true;
        }
      } else if (fd == ready_read.Get()) {
        char byte = 0;
        ssize_t bytes = 0;
        do {
          bytes = ::read(fd, &byte, 1);
        } while (bytes < 0 && errno == EINTR);
        if (bytes == 1) {
          ready = true;
        }
      }
    }

    if (ready) {
      break;
    }
  }

  if (ready) {
    const std::lock_guard<std::mutex> guard(mutex);
    if (stop_requested) {
      // 竞态：就绪与停止请求同时到达。停止受理是线性化点，不接受这次启动。
      // 先解锁再走统一 Stop()，避免在持锁时调用可能阻塞的收敛路径。
    } else {
      state = ChildProcessState::kRunning;
      ready_starts += 1;
      last_start_error = domain::Error{};
      return domain::Result<ChildProcessIdentity>::success(identity);
    }
  }

  // 走到这里只有两种可能：等待就绪超时，或就绪与停止请求竞态。统一停止并回收；如果
  // 停止本身失败，返回停止错误而不是 kTimeout，让调用方看到真实的资源状态。
  const bool cancelled = [this] {
    const std::lock_guard<std::mutex> guard(mutex);
    return stop_requested;
  }();
  const domain::Result<ChildProcessExit> stopped = Stop(identity, config.stop_wait_budget);
  if (!stopped.ok()) {
    const std::lock_guard<std::mutex> guard(mutex);
    last_start_error = stopped.error;
    return domain::Result<ChildProcessIdentity>::failure(stopped.error.code,
                                                         stopped.error.message);
  }
  const domain::Error error =
      cancelled ? MakeError(domain::ErrorCode::kCancelled, "子进程启动期间收到停止请求")
                : MakeError(domain::ErrorCode::kTimeout, "等待子进程就绪超出启动预算");
  {
    const std::lock_guard<std::mutex> guard(mutex);
    last_start_error = error;
  }
  return domain::Result<ChildProcessIdentity>::failure(error.code, error.message);
}

bool ChildProcess::Impl::RequestStop(ChildProcessIdentity expected) noexcept {
  try {
    const std::lock_guard<std::mutex> guard(mutex);
    if (!IdentityKnownLocked(expected) || !active_identity.valid()) {
      return false;
    }
    stop_requested = true;
    if (pid > 0 && !sigterm_sent) {
      (void)SendSignal(pid, SIGTERM, config.kill_process_group);
      sigterm_sent = true;
    }
    return true;
  } catch (...) {
    return false;
  }
}

domain::Result<ChildProcessExit> ChildProcess::Impl::Stop(
    ChildProcessIdentity expected, std::optional<std::chrono::milliseconds> budget) {
  const std::chrono::milliseconds grace = ClampBudget(budget.value_or(config.stop_wait_budget));
  {
    const std::lock_guard<std::mutex> guard(mutex);
    if (!IdentityKnownLocked(expected)) {
      return domain::Result<ChildProcessExit>::failure(
          domain::ErrorCode::kAlreadyCompleted, "停止请求针对的子进程身份已经过期");
    }
    if (!active_identity.valid()) {
      if (last_exit.kind != ChildProcessExitKind::kNone || !last_exit.error.ok()) {
        return domain::Result<ChildProcessExit>::success(last_exit);
      }
      return domain::Result<ChildProcessExit>::failure(
          domain::ErrorCode::kAlreadyCompleted, "子进程已经回收，没有可停止的活跃身份");
    }
    stop_requested = true;
    state = ChildProcessState::kStopping;
    if (pid > 0 && !sigterm_sent) {
      (void)SendSignal(pid, SIGTERM, config.kill_process_group);
      sigterm_sent = true;
    }
  }

  const auto graceful_deadline = std::chrono::steady_clock::now() + grace;
  while (true) {
    {
      const std::lock_guard<std::mutex> guard(mutex);
      if (ReapNonBlockingLocked()) {
        return domain::Result<ChildProcessExit>::success(last_exit);
      }
    }
    if (grace <= std::chrono::milliseconds::zero() ||
        std::chrono::steady_clock::now() >= graceful_deadline) {
      break;
    }
    std::this_thread::sleep_for(config.poll_interval);
  }

  {
    const std::lock_guard<std::mutex> guard(mutex);
    if (ReapNonBlockingLocked()) {
      return domain::Result<ChildProcessExit>::success(last_exit);
    }
    forced_kill = true;
    if (pid > 0) {
      (void)SendSignal(pid, SIGKILL, config.kill_process_group);
    }
  }

  const auto kill_deadline = std::chrono::steady_clock::now() + config.kill_wait_budget;
  while (true) {
    {
      const std::lock_guard<std::mutex> guard(mutex);
      if (ReapNonBlockingLocked()) {
        return domain::Result<ChildProcessExit>::success(last_exit);
      }
    }
    if (std::chrono::steady_clock::now() >= kill_deadline) {
      break;
    }
    std::this_thread::sleep_for(config.poll_interval);
  }

  {
    const std::lock_guard<std::mutex> guard(mutex);
    state = ChildProcessState::kUnavailable;
  }
  return domain::Result<ChildProcessExit>::failure(
      domain::ErrorCode::kTimeout, "子进程在强杀预算内仍未回收，槽位转为不可用");
}

domain::Result<ChildProcessExit> ChildProcess::Impl::WaitForExit(
    ChildProcessIdentity expected, std::optional<std::chrono::milliseconds> budget) {
  const bool bounded = budget.has_value();
  const std::chrono::milliseconds effective = bounded ? ClampBudget(budget.value())
                                                      : std::chrono::milliseconds::zero();
  const auto deadline = bounded ? std::chrono::steady_clock::now() + effective
                                : std::chrono::steady_clock::time_point::max();
  while (true) {
    {
      const std::lock_guard<std::mutex> guard(mutex);
      if (!IdentityKnownLocked(expected)) {
        return domain::Result<ChildProcessExit>::failure(
            domain::ErrorCode::kAlreadyCompleted, "等待请求针对的子进程身份已经过期");
      }
      if (ReapNonBlockingLocked()) {
        if (last_exit.kind != ChildProcessExitKind::kNone || !last_exit.error.ok()) {
          return domain::Result<ChildProcessExit>::success(last_exit);
        }
        return domain::Result<ChildProcessExit>::failure(
            domain::ErrorCode::kAlreadyCompleted, "子进程已经回收，没有可等待的活跃身份");
      }
    }
    if (bounded && std::chrono::steady_clock::now() >= deadline) {
      return domain::Result<ChildProcessExit>::failure(
          domain::ErrorCode::kTimeout, "等待子进程退出超出预算");
    }
    std::this_thread::sleep_for(config.poll_interval);
  }
}

ChildProcessStatus ChildProcess::Impl::Snapshot() {
  const std::lock_guard<std::mutex> guard(mutex);
  ReapNonBlockingLocked();
  ChildProcessStatus snapshot;
  snapshot.state = state;
  snapshot.identity = active_identity;
  snapshot.last_identity = last_identity;
  snapshot.process_alive = pid > 0;
  snapshot.stop_requested = stop_requested;
  snapshot.native_pid = pid > 0 ? static_cast<int>(pid) : static_cast<int>(last_pid);
  snapshot.start_attempts = start_attempts;
  snapshot.ready_starts = ready_starts;
  snapshot.clean_exits = clean_exits;
  snapshot.crash_exits = crash_exits;
  snapshot.forced_kills = forced_kills;
  snapshot.last_start_error = last_start_error;
  snapshot.last_exit = last_exit;
  return snapshot;
}

bool ChildProcess::Impl::Matches(ChildProcessIdentity identity) const noexcept {
  const std::lock_guard<std::mutex> guard(mutex);
  return active_identity.valid() && active_identity == identity;
}

void ChildProcess::Impl::Shutdown() {
  ChildProcessIdentity identity;
  {
    const std::lock_guard<std::mutex> guard(mutex);
    identity = active_identity;
    if (!identity.valid()) {
      return;
    }
    stop_requested = true;
  }
  const domain::Result<ChildProcessExit> stopped = Stop(identity, std::nullopt);
  (void)stopped;
}

ChildProcess::ChildProcess(ChildProcessConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

ChildProcess::~ChildProcess() = default;

domain::Result<ChildProcessIdentity> ChildProcess::start(const ChildProcessSpec& spec) {
  return impl_->Start(spec);
}

bool ChildProcess::request_stop(ChildProcessIdentity expected) noexcept {
  return impl_->RequestStop(expected);
}

domain::Result<ChildProcessExit> ChildProcess::stop(
    ChildProcessIdentity expected,
    std::optional<std::chrono::milliseconds> stop_wait_budget) {
  return impl_->Stop(expected, stop_wait_budget);
}

domain::Result<ChildProcessExit> ChildProcess::wait_for_exit(
    ChildProcessIdentity expected,
    std::optional<std::chrono::milliseconds> wait_budget) {
  return impl_->WaitForExit(expected, wait_budget);
}

ChildProcessStatus ChildProcess::status() const {
  return impl_->Snapshot();
}

bool ChildProcess::matches(ChildProcessIdentity identity) const noexcept {
  return impl_->Matches(identity);
}

}  // namespace nexweave::runtime

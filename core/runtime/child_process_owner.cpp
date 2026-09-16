#include "child_process_owner.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace nexweave::runtime {

namespace {

// 把子进程退出快照映射成拥有者运行结果。被停止或强杀视为取消语义下的正常收敛；自然
// 退出零码视为完成；其余退出和崩溃都是运行失败。
domain::OperationResult ExitToOperationResult(const ChildProcessExit& exit) {
  if (!exit.error.ok()) {
    return domain::OperationResult{exit.error};
  }
  switch (exit.kind) {
    case ChildProcessExitKind::kExited:
      if (exit.exit_code == 0) {
        return domain::OperationResult::success();
      }
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure,
          "子进程正常退出但返回码非零: " + std::to_string(exit.exit_code));
    case ChildProcessExitKind::kStopped:
    case ChildProcessExitKind::kKilled:
      return domain::OperationResult::success();
    case ChildProcessExitKind::kSignaled:
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure,
          "子进程被信号终止: " + std::to_string(exit.signal_number));
    case ChildProcessExitKind::kNone:
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                              "子进程退出原因不可用");
  }
  return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                          "子进程退出分类未知");
}

}  // namespace

// 一次会话的子进程拥有者。生命周期入口由 Supervisor 的同一个工作线程串行调用，因此
// 除 started_ 只在该线程读写外，不需要额外互斥量。request_stop() 只读 ChildProcess 的
// 当前身份并发送信号，不读写本对象的 started_。
class ChildProcessOwner final : public ISessionOwner {
 public:
  ChildProcessOwner(ChildProcessSpec spec, const ChildProcessConfig& config)
      : spec_(std::move(spec)),
        poll_interval_(config.poll_interval),
        process_(config) {}

  ~ChildProcessOwner() override = default;

  domain::OperationResult start() override {
    const domain::Result<ChildProcessIdentity> started = process_.start(spec_);
    if (!started.ok()) {
      return domain::OperationResult{started.error};
    }
    identity_ = started.value.value();
    started_ = true;
    return domain::OperationResult::success();
  }

  domain::OperationResult run() override {
    if (!started_) {
      return domain::OperationResult::failure(domain::ErrorCode::kAlreadyCompleted,
                                              "子进程拥有者尚未启动");
    }
    while (true) {
      const domain::Result<ChildProcessExit> waited =
          process_.wait_for_exit(identity_, poll_interval_);
      if (waited.ok()) {
        started_ = false;
        return ExitToOperationResult(waited.value.value());
      }
      if (waited.error.code != domain::ErrorCode::kTimeout) {
        started_ = false;
        return domain::OperationResult{waited.error};
      }

      const ChildProcessStatus snapshot = process_.status();
      if (!snapshot.stop_requested) {
        continue;
      }

      const domain::Result<ChildProcessExit> stopped = process_.stop(identity_, std::nullopt);
      if (!stopped.ok()) {
        // 保留 started_ = true，让 Supervisor 后续 cleanup() 还有机会再次收敛；
        // 若直接置 false，清理入口会误以为资源已经交还。
        return domain::OperationResult{stopped.error};
      }
      started_ = false;
      return ExitToOperationResult(stopped.value.value());
    }
  }

  domain::OperationResult cleanup() noexcept override {
    if (!started_) {
      return domain::OperationResult::success();
    }
    try {
      const domain::Result<ChildProcessExit> stopped = process_.stop(identity_, std::nullopt);
      if (!stopped.ok()) {
        return domain::OperationResult{stopped.error};
      }
      started_ = false;
      return domain::OperationResult::success();
    } catch (...) {
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                              "清理子进程时发生异常");
    }
  }

  void request_stop() noexcept override {
    // 传 0 表示当前活跃身份。拥有者不会被复用，所以这里不需要读可能尚未发布的 identity_。
    (void)process_.request_stop(ChildProcessIdentity{});
  }

 private:
  ChildProcessSpec spec_;
  // 由构造时冻结的轮询间隔；ChildProcess 不暴露配置读取接口，run() 用它做有界观察。
  std::chrono::milliseconds poll_interval_{10};
  ChildProcess process_;
  ChildProcessIdentity identity_{};
  bool started_ = false;
};

ChildProcessOwnerFactory::ChildProcessOwnerFactory(ChildProcessSpec spec, ChildProcessConfig config)
    : spec_(std::move(spec)), config_(std::move(config)) {
  const domain::OperationResult spec_valid = validate_child_process_spec(spec_);
  const domain::OperationResult config_valid = validate_child_process_config(config_);
  if (!spec_valid.ok()) {
    config_error_ = spec_valid.error;
  } else if (!config_valid.ok()) {
    config_error_ = config_valid.error;
  }
}

std::shared_ptr<ISessionOwner> ChildProcessOwnerFactory::create(
    const SupervisorSessionSpec& spec, domain::Error& error) {
  (void)spec;
  if (!config_error_.ok()) {
    error = config_error_;
    return nullptr;
  }
  try {
    return std::make_shared<ChildProcessOwner>(spec_, config_);
  } catch (...) {
    error = domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                             "创建子进程拥有者时发生异常")
                .error;
    return nullptr;
  }
}

}  // namespace nexweave::runtime

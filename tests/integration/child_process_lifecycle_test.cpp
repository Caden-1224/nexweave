#include "../test_support.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

#include "child_process.hpp"

using namespace nexweave;
using namespace std::chrono_literals;

#ifndef NEXWEAVE_CHILD_PROCESS_HELPER
#error "测试需要 NEXWEAVE_CHILD_PROCESS_HELPER 指向子进程夹具可执行文件"
#endif

namespace {

std::string MakeTempPath() {
  char path[] = "/tmp/nexweave-child-XXXXXX";
  const int fd = ::mkstemp(path);
  if (fd >= 0) {
    ::close(fd);
  }
  std::error_code error;
  std::filesystem::remove(path, error);
  return path;
}

runtime::ChildProcessSpec SpecFor(const std::string& mode,
                                  std::initializer_list<std::string> arguments = {}) {
  runtime::ChildProcessSpec spec;
  spec.executable = NEXWEAVE_CHILD_PROCESS_HELPER;
  spec.arguments.push_back(mode);
  for (const std::string& argument : arguments) {
    spec.arguments.push_back(argument);
  }
  return spec;
}

runtime::ChildProcessConfig TightConfig(
    std::chrono::milliseconds start = 1000ms,
    std::chrono::milliseconds stop = 200ms,
    std::chrono::milliseconds kill = 500ms,
    std::chrono::milliseconds poll = 2ms) {
  runtime::ChildProcessConfig config;
  config.start_wait_budget = start;
  config.stop_wait_budget = stop;
  config.kill_wait_budget = kill;
  config.poll_interval = poll;
  return config;
}

bool WaitForState(const runtime::ChildProcess& process, runtime::ChildProcessState state,
                  std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (process.status().state == state) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return process.status().state == state;
}

bool ProcessGone(int pid) {
  if (pid <= 0) {
    return true;
  }
  errno = 0;
  const int result = ::kill(pid, 0);
  return result != 0 && errno == ESRCH;
}

bool WaitForPidFile(const std::string& path, int* pid) {
  const auto deadline = std::chrono::steady_clock::now() + 1000ms;
  while (std::chrono::steady_clock::now() < deadline) {
    std::ifstream file(path);
    if (file.is_open()) {
      file >> *pid;
      if (*pid > 0) {
        return true;
      }
    }
    std::this_thread::sleep_for(2ms);
  }
  return false;
}

void StopChild(runtime::ChildProcess* process, runtime::ChildProcessIdentity identity,
               std::chrono::milliseconds budget = 500ms) {
  const auto stopped = process->stop(identity, budget);
  CHECK(stopped.ok());
}

void TestStartWaitsForReadyNotification() {
  runtime::ChildProcess process(TightConfig());
  const std::string trigger = MakeTempPath();
  const runtime::ChildProcessSpec spec = SpecFor("trigger-ready", {trigger});

  auto future = std::async(std::launch::async, [&process, &spec] {
    return process.start(spec);
  });
  // 触发文件尚未出现，start 必须停在就绪等待，不能因为进程已经 fork 就提前返回。
  CHECK(future.wait_for(30ms) == std::future_status::timeout);

  {
    std::ofstream trigger_file(trigger);
    trigger_file << 'x';
  }

  const domain::Result<runtime::ChildProcessIdentity> started = future.get();
  CHECK(started.ok());
  CHECK(started.value->valid());

  const runtime::ChildProcessStatus status = process.status();
  CHECK(status.state == runtime::ChildProcessState::kRunning);
  CHECK(status.process_alive);
  CHECK(status.native_pid > 0);
  CHECK(status.identity == *started.value);

  const auto stopped = process.stop(*started.value, 500ms);
  CHECK(stopped.ok());
  CHECK(stopped.value->kind == runtime::ChildProcessExitKind::kStopped);
  CHECK(!stopped.value->forced);
  CHECK(ProcessGone(status.native_pid));
  CHECK(process.status().slot_reusable());
  std::filesystem::remove(trigger);
}

void TestExecFailureRollsBackAndAllowsRetry() {
  runtime::ChildProcess process(TightConfig());
  runtime::ChildProcessSpec bad = SpecFor("ready");
  bad.executable = "/tmp/nexweave-child-process-does-not-exist";
  const auto failed = process.start(bad);
  CHECK(!failed.ok());
  CHECK(failed.error.code == domain::ErrorCode::kBackendFailure);

  const runtime::ChildProcessStatus after_failure = process.status();
  CHECK(after_failure.slot_reusable());
  CHECK(!after_failure.last_start_error.ok());
  CHECK(after_failure.start_attempts == 1);

  const auto started = process.start(SpecFor("ready"));
  CHECK(started.ok());
  CHECK(process.status().state == runtime::ChildProcessState::kRunning);
  StopChild(&process, *started.value);
}

void TestStartTimeoutKillsIgnoringChildAndReaps() {
  runtime::ChildProcess process(TightConfig(80ms, 50ms, 500ms, 2ms));
  const auto failed = process.start(SpecFor("never-ready-ignore"));
  CHECK(!failed.ok());
  CHECK(failed.error.code == domain::ErrorCode::kTimeout);

  const runtime::ChildProcessStatus status = process.status();
  // 夹具忽略 SIGTERM；实现必须升级 SIGKILL 并回收，因此槽位回到空闲，而不是留下僵尸。
  CHECK(status.slot_reusable());
  CHECK(status.last_exit.kind == runtime::ChildProcessExitKind::kKilled);
  CHECK(status.last_exit.forced);
  CHECK(status.last_exit.stop_requested);
  CHECK(status.native_pid > 0);
  CHECK(ProcessGone(status.native_pid));

  const auto restarted = process.start(SpecFor("ready"));
  CHECK(restarted.ok());
  StopChild(&process, *restarted.value);
}

void TestRequestStopDuringStartupCancels() {
  runtime::ChildProcess process(TightConfig(2000ms, 100ms, 500ms, 2ms));
  const std::string trigger = MakeTempPath();
  const runtime::ChildProcessSpec spec = SpecFor("trigger-ready", {trigger});

  auto future = std::async(std::launch::async, [&process, &spec] {
    return process.start(spec);
  });
  CHECK(WaitForState(process, runtime::ChildProcessState::kStarting, 500ms));
  CHECK(process.request_stop(runtime::ChildProcessIdentity{}));

  const auto cancelled = future.get();
  CHECK(!cancelled.ok());
  CHECK(cancelled.error.code == domain::ErrorCode::kCancelled);

  const runtime::ChildProcessStatus status = process.status();
  CHECK(status.slot_reusable());
  CHECK(ProcessGone(status.native_pid));
  std::filesystem::remove(trigger);
}

void TestStopIsCooperativeAndIdempotent() {
  runtime::ChildProcess process(TightConfig());
  const auto started = process.start(SpecFor("ready"));
  CHECK(started.ok());
  const runtime::ChildProcessIdentity identity = *started.value;

  CHECK(process.request_stop(identity));
  const auto stopped = process.stop(identity, 500ms);
  CHECK(stopped.ok());
  CHECK(stopped.value->kind == runtime::ChildProcessExitKind::kStopped);
  CHECK(stopped.value->stop_requested);
  CHECK(!stopped.value->forced);

  const auto repeated = process.stop(identity, 0ms);
  CHECK(repeated.ok());
  CHECK(repeated.value->kind == stopped.value->kind);
  CHECK(!process.request_stop(identity));
  CHECK(process.status().slot_reusable());
}

void TestStopEscalatesToKillWhenSigtermIgnored() {
  runtime::ChildProcess process(TightConfig(1000ms, 80ms, 500ms, 2ms));
  const auto started = process.start(SpecFor("ready-ignore-sigterm"));
  CHECK(started.ok());
  const runtime::ChildProcessIdentity identity = *started.value;

  const auto stopped = process.stop(identity, 80ms);
  CHECK(stopped.ok());
  CHECK(stopped.value->kind == runtime::ChildProcessExitKind::kKilled);
  CHECK(stopped.value->forced);
  CHECK(stopped.value->stop_requested);

  const runtime::ChildProcessStatus status = process.status();
  CHECK(ProcessGone(status.native_pid));
  CHECK(status.forced_kills == 1);
  CHECK(process.stop(identity, 0ms).ok());
}

void TestNaturalExitAndCrashArchives() {
  runtime::ChildProcess process(TightConfig());
  {
    const auto started = process.start(SpecFor("ready-exit", {"7"}));
    CHECK(started.ok());
    const auto exited = process.wait_for_exit(*started.value, 1000ms);
    CHECK(exited.ok());
    CHECK(exited.value->kind == runtime::ChildProcessExitKind::kExited);
    CHECK(exited.value->exit_code == 7);
    CHECK(!exited.value->stop_requested);
    CHECK(!exited.value->forced);
  }
  CHECK(process.status().clean_exits == 0);

  {
    const auto started = process.start(SpecFor("ready-crash"));
    CHECK(started.ok());
    const auto crashed = process.wait_for_exit(*started.value, 1000ms);
    CHECK(crashed.ok());
    CHECK(crashed.value->kind == runtime::ChildProcessExitKind::kSignaled);
    CHECK(crashed.value->signal_number == SIGABRT);
    CHECK(process.status().crash_exits == 1);
  }
}

void TestStaleIdentityRejectedAfterRebuild() {
  runtime::ChildProcess process(TightConfig());
  const auto first = process.start(SpecFor("ready"));
  CHECK(first.ok());
  const runtime::ChildProcessIdentity old_identity = *first.value;
  StopChild(&process, old_identity);

  const auto second = process.start(SpecFor("ready"));
  CHECK(second.ok());
  CHECK(second.value->value != old_identity.value);
  const runtime::ChildProcessIdentity new_identity = *second.value;

  const auto old_wait = process.wait_for_exit(old_identity, 0ms);
  CHECK(!old_wait.ok());
  CHECK(old_wait.error.code == domain::ErrorCode::kAlreadyCompleted);

  const auto old_stop = process.stop(old_identity, 0ms);
  CHECK(!old_stop.ok());
  CHECK(old_stop.error.code == domain::ErrorCode::kAlreadyCompleted);

  CHECK(!process.request_stop(old_identity));
  CHECK(process.matches(new_identity));
  CHECK(!process.matches(old_identity));
  CHECK(process.status().state == runtime::ChildProcessState::kRunning);

  StopChild(&process, new_identity);
}

void TestExitBeforeReadyRollsBack() {
  runtime::ChildProcess process(TightConfig());
  const auto failed = process.start(SpecFor("exit-before-ready"));
  CHECK(!failed.ok());
  CHECK(failed.error.code == domain::ErrorCode::kBackendFailure);
  const runtime::ChildProcessStatus status = process.status();
  CHECK(status.slot_reusable());
  CHECK(!status.last_start_error.ok());
  CHECK(ProcessGone(status.native_pid));
}

void TestZeroExitCountsAsCleanExit() {
  runtime::ChildProcess process(TightConfig());
  const auto started = process.start(SpecFor("ready-exit", {"0"}));
  CHECK(started.ok());
  const auto exited = process.wait_for_exit(*started.value, 1000ms);
  CHECK(exited.ok());
  CHECK(exited.value->kind == runtime::ChildProcessExitKind::kExited);
  CHECK(exited.value->exit_code == 0);
  CHECK(process.status().clean_exits == 1);
  CHECK(process.status().slot_reusable());
}

void TestProcessGroupTerminatesGrandchild() {
  runtime::ChildProcess process(TightConfig(1000ms, 200ms, 500ms, 2ms));
  const std::string pid_file = MakeTempPath();
  const auto started = process.start(SpecFor("ready-spawn-grandchild", {pid_file}));
  CHECK(started.ok());

  int grandchild_pid = 0;
  CHECK(WaitForPidFile(pid_file, &grandchild_pid));
  CHECK(grandchild_pid > 0);
  CHECK(!ProcessGone(grandchild_pid));

  const auto stopped = process.stop(*started.value, 500ms);
  CHECK(stopped.ok());
  CHECK(stopped.value->kind == runtime::ChildProcessExitKind::kStopped);
  CHECK(ProcessGone(grandchild_pid));
  std::filesystem::remove(pid_file);
}

void TestBusyStartRejectedWithoutSideEffect() {
  runtime::ChildProcess process(TightConfig());
  const auto first = process.start(SpecFor("ready"));
  CHECK(first.ok());

  const auto second = process.start(SpecFor("ready"));
  CHECK(!second.ok());
  CHECK(second.error.code == domain::ErrorCode::kBusy);
  CHECK(process.status().start_attempts == 1);
  CHECK(process.matches(*first.value));

  StopChild(&process, *first.value);
}

void TestRepeatedStartStopDoesNotReuseIdentity() {
  runtime::ChildProcess process(TightConfig());
  std::uint64_t last_identity = 0;
  for (int round = 0; round < 3; ++round) {
    const auto started = process.start(SpecFor("ready"));
    CHECK(started.ok());
    CHECK(started.value->value > last_identity);
    last_identity = started.value->value;
    const int pid = process.status().native_pid;
    StopChild(&process, *started.value);
    CHECK(ProcessGone(pid));
    CHECK(process.status().slot_reusable());
  }
  CHECK(process.status().start_attempts == 3);
  CHECK(process.status().ready_starts == 3);
}

}  // namespace

int main() {
  TestStartWaitsForReadyNotification();
  TestExecFailureRollsBackAndAllowsRetry();
  TestStartTimeoutKillsIgnoringChildAndReaps();
  TestRequestStopDuringStartupCancels();
  TestStopIsCooperativeAndIdempotent();
  TestStopEscalatesToKillWhenSigtermIgnored();
  TestNaturalExitAndCrashArchives();
  TestStaleIdentityRejectedAfterRebuild();
  TestExitBeforeReadyRollsBack();
  TestZeroExitCountsAsCleanExit();
  TestProcessGroupTerminatesGrandchild();
  TestBusyStartRejectedWithoutSideEffect();
  TestRepeatedStartStopDoesNotReuseIdentity();
  std::cout << "child_process_lifecycle_test 全部通过" << std::endl;
  return 0;
}

#include "../test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <unistd.h>

#include "child_process_owner.hpp"

using namespace nexweave;
using namespace std::chrono_literals;

#ifndef NEXWEAVE_CHILD_PROCESS_HELPER
#error "测试需要 NEXWEAVE_CHILD_PROCESS_HELPER 指向子进程夹具可执行文件"
#endif

namespace {

std::string MakeTempPath() {
  char path[] = "/tmp/nexweave-owner-XXXXXX";
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

runtime::ChildProcessConfig ChildConfig() {
  runtime::ChildProcessConfig config;
  config.start_wait_budget = 500ms;
  config.stop_wait_budget = 100ms;
  config.kill_wait_budget = 300ms;
  config.poll_interval = 2ms;
  return config;
}

runtime::SupervisorConfig SupervisorConfig() {
  runtime::SupervisorConfig config;
  config.start_wait_budget = 1500ms;
  config.cleanup_wait_budget = 1500ms;
  return config;
}

runtime::SupervisorSessionSpec SessionSpec(const std::string& id) {
  runtime::SupervisorSessionSpec spec;
  spec.work_id = "work-" + id;
  spec.request_id = "request-" + id;
  return spec;
}

void TestStartWaitsForChildReadyAndCancelConverges() {
  const std::string trigger = MakeTempPath();
  runtime::ChildProcessOwnerFactory trigger_factory(SpecFor("trigger-ready", {trigger}),
                                                   ChildConfig());
  runtime::Supervisor trigger_supervisor(trigger_factory, SupervisorConfig());

  auto future = std::async(std::launch::async, [&trigger_supervisor] {
    return trigger_supervisor.start(SessionSpec("ready"));
  });
  // start 必须停在子进程就绪之前，不能因为拥有者已经创建就返回成功。
  CHECK(future.wait_for(40ms) == std::future_status::timeout);
  {
    std::ofstream trigger_file(trigger);
    trigger_file << 'x';
  }
  const domain::OperationResult started = future.get();
  CHECK(started.ok());

  const runtime::SupervisorStatus active = trigger_supervisor.status();
  CHECK(active.state == runtime::SupervisorState::kActive);
  CHECK(active.session_sequence == 1);

  const runtime::SupervisorControlOutcome cancelled =
      trigger_supervisor.cancel(active.session_sequence, 1000ms);
  CHECK(cancelled.accepted);
  CHECK(cancelled.cleanup_completed);
  CHECK(cancelled.error.ok());
  CHECK(trigger_supervisor.status().slot_reusable());
  CHECK(trigger_supervisor.status().sessions_cancelled == 1);

  // 同一个工厂换一轮会话：新的子进程身份与新的会话序号都不能复用上一轮。
  const domain::OperationResult restarted = trigger_supervisor.start(SessionSpec("ready-2"));
  CHECK(restarted.ok());
  const runtime::SupervisorStatus second = trigger_supervisor.status();
  CHECK(second.session_sequence == 2);
  const auto second_cancel = trigger_supervisor.cancel(second.session_sequence, 1000ms);
  CHECK(second_cancel.cleanup_completed);
  CHECK(trigger_supervisor.status().sessions_cancelled == 2);
  std::filesystem::remove(trigger);
}

void TestNaturalExitCompletesSession() {
  runtime::ChildProcessOwnerFactory factory(SpecFor("ready-exit", {"0"}), ChildConfig());
  runtime::Supervisor supervisor(factory, SupervisorConfig());
  const auto started = supervisor.start(SessionSpec("normal"));
  CHECK(started.ok());
  CHECK(supervisor.wait_for_slot(1000ms));
  const runtime::SupervisorStatus status = supervisor.status();
  CHECK(status.slot_reusable());
  CHECK(status.sessions_started == 1);
  CHECK(status.sessions_completed == 1);
  CHECK(status.last_run_error.ok());
  CHECK(status.last_cleanup_error.ok());
}

void TestCrashMarksSessionFailed() {
  runtime::ChildProcessOwnerFactory factory(SpecFor("ready-crash"), ChildConfig());
  runtime::Supervisor supervisor(factory, SupervisorConfig());
  const auto started = supervisor.start(SessionSpec("crash"));
  CHECK(started.ok());
  CHECK(supervisor.wait_for_slot(1000ms));
  const runtime::SupervisorStatus status = supervisor.status();
  CHECK(status.sessions_failed == 1);
  CHECK(status.last_run_error.code == domain::ErrorCode::kBackendFailure);
  CHECK(status.slot_reusable());
}

void TestStartTimeoutRollsBackChild() {
  runtime::ChildProcessConfig child_config = ChildConfig();
  child_config.start_wait_budget = 80ms;
  runtime::ChildProcessOwnerFactory factory(SpecFor("never-ready-ignore"), child_config);
  runtime::Supervisor supervisor(factory, SupervisorConfig());
  const auto started = supervisor.start(SessionSpec("timeout"));
  CHECK(!started.ok());
  CHECK(started.error.code == domain::ErrorCode::kTimeout);
  CHECK(supervisor.wait_for_slot(1000ms));
  const runtime::SupervisorStatus status = supervisor.status();
  CHECK(status.slot_reusable());
  CHECK(status.last_start_error.code == domain::ErrorCode::kTimeout);
  CHECK(status.sessions_failed == 1);
}

void TestCancelEscalatesForIgnoringChild() {
  runtime::ChildProcessOwnerFactory factory(SpecFor("ready-ignore-sigterm"), ChildConfig());
  runtime::Supervisor supervisor(factory, SupervisorConfig());
  const auto started = supervisor.start(SessionSpec("ignore"));
  CHECK(started.ok());
  const runtime::SupervisorControlOutcome cancelled =
      supervisor.cancel(supervisor.status().session_sequence, 1000ms);
  CHECK(cancelled.accepted);
  CHECK(cancelled.cleanup_completed);
  CHECK(cancelled.error.ok());
  const runtime::SupervisorStatus status = supervisor.status();
  CHECK(status.slot_reusable());
  CHECK(status.sessions_cancelled == 1);
  CHECK(status.last_cleanup_error.ok());
}

}  // namespace

int main() {
  TestStartWaitsForChildReadyAndCancelConverges();
  TestNaturalExitCompletesSession();
  TestCrashMarksSessionFailed();
  TestStartTimeoutRollsBackChild();
  TestCancelEscalatesForIgnoringChild();
  return 0;
}

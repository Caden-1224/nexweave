#include "../test_support.hpp"

#include <chrono>
#include <string>

#include "child_process.hpp"

using namespace nexweave;
using namespace std::chrono_literals;

namespace {

void TestIdentityAndNames() {
  runtime::ChildProcessIdentity invalid;
  runtime::ChildProcessIdentity valid{42};
  CHECK(!invalid.valid());
  CHECK(valid.valid());
  CHECK(valid == runtime::ChildProcessIdentity{42});
  CHECK(valid != invalid);

  CHECK(std::string(runtime::to_string(runtime::ChildProcessState::kIdle)) == "idle");
  CHECK(std::string(runtime::to_string(runtime::ChildProcessState::kStarting)) == "starting");
  CHECK(std::string(runtime::to_string(runtime::ChildProcessState::kRunning)) == "running");
  CHECK(std::string(runtime::to_string(runtime::ChildProcessState::kStopping)) == "stopping");
  CHECK(std::string(runtime::to_string(runtime::ChildProcessState::kUnavailable)) ==
        "unavailable");
  CHECK(std::string(runtime::to_string(runtime::ChildProcessExitKind::kExited)) == "exited");
  CHECK(std::string(runtime::to_string(runtime::ChildProcessExitKind::kSignaled)) == "signaled");
  CHECK(std::string(runtime::to_string(runtime::ChildProcessExitKind::kStopped)) == "stopped");
  CHECK(std::string(runtime::to_string(runtime::ChildProcessExitKind::kKilled)) == "killed");
}

void TestConfigValidation() {
  CHECK(runtime::validate_child_process_config(runtime::ChildProcessConfig{}).ok());

  runtime::ChildProcessConfig config;
  config.start_wait_budget = 0ms;
  CHECK(!runtime::validate_child_process_config(config).ok());
  CHECK(runtime::validate_child_process_config(config).error.code ==
        domain::ErrorCode::kInvalidInput);
  config = runtime::ChildProcessConfig{};
  config.stop_wait_budget = 0ms;
  CHECK(!runtime::validate_child_process_config(config).ok());
  config = runtime::ChildProcessConfig{};
  config.kill_wait_budget = 0ms;
  CHECK(!runtime::validate_child_process_config(config).ok());
  config = runtime::ChildProcessConfig{};
  config.poll_interval = 0ms;
  CHECK(!runtime::validate_child_process_config(config).ok());
}

void TestSpecValidation() {
  runtime::ChildProcessSpec spec;
  CHECK(!runtime::validate_child_process_spec(spec).ok());
  CHECK(runtime::validate_child_process_spec(spec).error.code ==
        domain::ErrorCode::kInvalidInput);

  spec.executable = "/bin/true";
  CHECK(runtime::validate_child_process_spec(spec).ok());

  spec.arguments.push_back(std::string("bad\0arg", 7));
  CHECK(!runtime::validate_child_process_spec(spec).ok());

  spec.arguments.clear();
  spec.environment.push_back("NO_EQUALS");
  CHECK(!runtime::validate_child_process_spec(spec).ok());

  spec.environment.clear();
  spec.environment.push_back("=EMPTY_KEY");
  CHECK(!runtime::validate_child_process_spec(spec).ok());

  spec.environment.clear();
  spec.environment.push_back("KEY=value");
  CHECK(runtime::validate_child_process_spec(spec).ok());
}

}  // namespace

int main() {
  TestIdentityAndNames();
  TestConfigValidation();
  TestSpecValidation();
  return 0;
}

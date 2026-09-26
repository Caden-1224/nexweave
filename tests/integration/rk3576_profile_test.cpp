// RK3576 多进程部署 profile 集成测试：真实 fork/exec、就绪管道、音频所有权与回滚顺序。
//
// 使用受控子进程夹具模拟模型/设备节点，不加载真实模型，也不打开 ALSA。用例共同保护：
//   - 启动严格等各节点显式就绪，并先让音频拥有者就绪，再启动适配器消费者；
//   - 一个音频拥有者独占 lock/socket，消费者只能连接该适配器，不能直接争抢所有权；
//   - 重复 start/stop/restart 不残留子进程、lock/socket 或就绪资源；
//   - 任一节点启动失败或超时，之前已经就绪的节点必须按反序回滚；
//   - 已就绪节点异常退出或忽略 SIGTERM 时，stop() 仍有界收敛并释放拥有者资源；
//   - 非法拓扑在 fork 前被拒绝。
#include "rk3576_profile.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef NEXWEAVE_DEPLOYMENT_NODE_FIXTURE
#error "测试需要 NEXWEAVE_DEPLOYMENT_NODE_FIXTURE 指向部署节点夹具"
#endif

namespace {

using namespace std::chrono_literals;
using nexweave::app::Rk3576Profile;
using nexweave::app::Rk3576ProfileConfig;
using nexweave::app::Rk3576ProfileState;
using nexweave::app::Rk3576ProcessSpec;
using nexweave::domain::ErrorCode;

std::string MakeTempDirectory() {
  char path[] = "/tmp/nexweave-rk3576-profile-XXXXXX";
  char* directory = ::mkdtemp(path);
  if (directory == nullptr) {
    throw std::runtime_error("无法创建临时目录");
  }
  return directory;
}

class TempDirectory {
 public:
  TempDirectory() : path_(MakeTempDirectory()) {}
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::string& path() const {
    return path_;
  }

 private:
  std::string path_;
};

Rk3576ProcessSpec MakeNode(const std::string& name,
                           const std::string& adapter_path,
                           const std::string& mode,
                           bool owns_audio,
                           bool uses_audio_adapter,
                           std::chrono::milliseconds start_budget = 1000ms) {
  Rk3576ProcessSpec spec;
  spec.name = name;
  spec.process.executable = NEXWEAVE_DEPLOYMENT_NODE_FIXTURE;
  spec.process.arguments = {"--name", name, "--adapter", adapter_path, "--mode", mode};
  if (owns_audio) {
    spec.process.arguments.push_back("--owns-audio");
  }
  if (uses_audio_adapter) {
    spec.process.arguments.push_back("--uses-audio-adapter");
  }
  spec.process.expect_ready_signal = true;
  spec.lifecycle.start_wait_budget = start_budget;
  spec.lifecycle.stop_wait_budget = 200ms;
  spec.lifecycle.kill_wait_budget = 500ms;
  spec.lifecycle.poll_interval = 2ms;
  spec.owns_audio_device = owns_audio;
  spec.uses_audio_adapter = uses_audio_adapter;
  return spec;
}

Rk3576ProfileConfig MakeValidConfig(const std::string& adapter_path) {
  Rk3576ProfileConfig config;
  Rk3576ProcessSpec owner = MakeNode("audio-frontend", adapter_path, "hold", true, false);
  Rk3576ProcessSpec asr = MakeNode("asr", adapter_path, "hold", false, true);
  asr.start_after = {"audio-frontend"};
  Rk3576ProcessSpec llm = MakeNode("llm", adapter_path, "hold", false, true);
  llm.start_after = {"audio-frontend"};
  Rk3576ProcessSpec tts = MakeNode("tts", adapter_path, "hold", false, true);
  tts.start_after = {"audio-frontend"};
  Rk3576ProcessSpec session = MakeNode("session", adapter_path, "hold", false, true);
  session.start_after = {"asr", "llm", "tts"};
  config.processes = {std::move(owner), std::move(asr), std::move(llm), std::move(tts),
                      std::move(session)};
  return config;
}

bool ProcessReady(const Rk3576Profile& profile, const std::string& name) {
  for (const auto& process : profile.status().processes) {
    if (process.name == name) {
      return process.ready;
    }
  }
  return false;
}

// 保护不变量：音频拥有者先取得独占 lock/socket 并报告就绪，消费者再按依赖启动；stop() 后
// 所有进程和测试资源都必须消失，重复 stop() 幂等。
void TestStartReadyStopAndAudioOwnership() {
  TempDirectory temp;
  const std::string adapter_path = temp.path() + "/audio-frontend";
  Rk3576Profile profile(MakeValidConfig(adapter_path));

  CHECK(profile.start().ok());
  CHECK(profile.running());
  const auto status = profile.status();
  CHECK(status.state == Rk3576ProfileState::kReady);
  CHECK(status.audio_owner_name == "audio-frontend");
  CHECK(status.audio_owner_ready);
  CHECK(status.audio_adapter_ready);
  for (const auto& process : status.processes) {
    CHECK(process.ready);
  }
  CHECK(std::filesystem::exists(adapter_path + ".lock"));
  CHECK(std::filesystem::exists(adapter_path + ".sock"));

  const auto busy = profile.start();
  CHECK(!busy.ok());
  CHECK(busy.error.code == ErrorCode::kBusy);

  CHECK(profile.stop().ok());
  const auto stopped = profile.status();
  CHECK(stopped.state == Rk3576ProfileState::kIdle);
  CHECK(!stopped.running);
  for (const auto& process : stopped.processes) {
    CHECK(!process.ready);
  }
  CHECK(!std::filesystem::exists(adapter_path + ".lock"));
  CHECK(!std::filesystem::exists(adapter_path + ".sock"));
  CHECK(profile.stop().ok());
}

// 保护不变量：重复启停和 restart 不会累积子进程或资源；每一轮都重新创建拥有者和适配器
// 消费者，而不是复用上一轮残留状态。
void TestRepeatedStartStopRestartAndCleanup() {
  TempDirectory temp;
  const std::string adapter_path = temp.path() + "/audio-frontend";
  Rk3576Profile profile(MakeValidConfig(adapter_path));

  for (int round = 0; round < 5; ++round) {
    if (round == 0) {
      CHECK(profile.start().ok());
    } else {
      CHECK(profile.restart().ok());
    }
    CHECK(ProcessReady(profile, "audio-frontend"));
    CHECK(ProcessReady(profile, "session"));
    CHECK(profile.stop().ok());
    CHECK(!std::filesystem::exists(adapter_path + ".lock"));
    CHECK(!std::filesystem::exists(adapter_path + ".sock"));
  }

  const auto status = profile.status();
  CHECK(status.state == Rk3576ProfileState::kIdle);
  CHECK(status.start_attempts == 5);
  CHECK(status.starts_succeeded == 5);
  CHECK(status.start_failures == 0);
  CHECK(status.stop_requests == 9);
  CHECK(status.stops_succeeded == 9);
  CHECK(status.restarts_succeeded == 4);
}

// 保护不变量：下游节点启动失败时，之前已经就绪的音频拥有者和适配器消费者全部回滚；
// 失败不会被误报为就绪，失败节点保留结构化错误，后续 stop() 仍可重复执行。
void TestPartialStartFailureRollsBack() {
  TempDirectory temp;
  const std::string adapter_path = temp.path() + "/audio-frontend";
  Rk3576ProfileConfig config;
  Rk3576ProcessSpec owner = MakeNode("audio-frontend", adapter_path, "hold", true, false);
  Rk3576ProcessSpec failing = MakeNode("asr", adapter_path, "exit-before-ready", false, true);
  failing.start_after = {"audio-frontend"};
  Rk3576ProcessSpec session = MakeNode("session", adapter_path, "hold", false, true);
  session.start_after = {"asr"};
  config.processes = {std::move(owner), std::move(failing), std::move(session)};

  Rk3576Profile profile(std::move(config));
  const auto started = profile.start();
  CHECK(!started.ok());
  CHECK(started.error.code == ErrorCode::kBackendFailure);
  const auto status = profile.status();
  CHECK(status.state == Rk3576ProfileState::kIdle);
  CHECK(!status.running);
  CHECK(!std::filesystem::exists(adapter_path + ".lock"));
  CHECK(!std::filesystem::exists(adapter_path + ".sock"));
  CHECK(status.start_failures == 1);
  CHECK(status.processes[0].started == false);
  CHECK(status.processes[0].ready == false);
  CHECK(status.processes[1].started == false);
  CHECK(!status.processes[1].start_error.ok());
  CHECK(status.processes[2].started == false);
  CHECK(profile.stop().ok());
}

// 保护不变量：节点不在启动预算内报告就绪时，启动返回 kTimeout，并回收该节点和之前所有
// 已就绪节点；拥有者资源不能留给下一轮造成“设备已经被占用”的假象。
void TestStartTimeoutRollsBack() {
  TempDirectory temp;
  const std::string adapter_path = temp.path() + "/audio-frontend";
  Rk3576ProfileConfig config;
  Rk3576ProcessSpec owner = MakeNode("audio-frontend", adapter_path, "hold", true, false);
  Rk3576ProcessSpec slow = MakeNode("slow", adapter_path, "no-ready", false, true, 120ms);
  slow.start_after = {"audio-frontend"};
  slow.lifecycle.stop_wait_budget = 80ms;
  slow.lifecycle.kill_wait_budget = 500ms;
  config.processes = {std::move(owner), std::move(slow)};

  Rk3576Profile profile(std::move(config));
  const auto started = profile.start();
  CHECK(!started.ok());
  CHECK(started.error.code == ErrorCode::kTimeout);
  const auto status = profile.status();
  CHECK(status.state == Rk3576ProfileState::kIdle);
  CHECK(!status.running);
  CHECK(!std::filesystem::exists(adapter_path + ".lock"));
  CHECK(!std::filesystem::exists(adapter_path + ".sock"));
  CHECK(profile.stop().ok());
}

// 保护不变量：已就绪节点异常退出后 stop() 仍能回收其余节点；不会因为一个进程先退出就
// 跳过拥有者释放，也不会无限等待一个已经结束的身份。
void TestCrashAfterReadyStillStops() {
  TempDirectory temp;
  const std::string adapter_path = temp.path() + "/audio-frontend";
  Rk3576ProfileConfig config = MakeValidConfig(adapter_path);
  config.processes[1] = MakeNode("asr", adapter_path, "crash-after-ready", false, true);
  config.processes[1].start_after = {"audio-frontend"};

  Rk3576Profile profile(std::move(config));
  CHECK(profile.start().ok());

  bool crashed = false;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!ProcessReady(profile, "asr")) {
      crashed = true;
      break;
    }
    std::this_thread::sleep_for(5ms);
  }
  CHECK_MESSAGE(crashed, "asr 节点没有在就绪后退出");
  CHECK(profile.stop().ok());
  CHECK(profile.status().state == Rk3576ProfileState::kIdle);
  CHECK(!std::filesystem::exists(adapter_path + ".lock"));
  CHECK(!std::filesystem::exists(adapter_path + ".sock"));
}

// 保护不变量：节点忽略 SIGTERM 时，停止预算到期后仍会升级 SIGKILL 并回收；拥有者最后
// 停止，因此适配器 lock/socket 在整组停止完成后仍被正常释放。
void TestStopEscalatesToKill() {
  TempDirectory temp;
  const std::string adapter_path = temp.path() + "/audio-frontend";
  Rk3576ProfileConfig config;
  Rk3576ProcessSpec owner = MakeNode("audio-frontend", adapter_path, "hold", true, false);
  Rk3576ProcessSpec stubborn =
      MakeNode("stubborn", adapter_path, "ignore-term", false, true);
  stubborn.start_after = {"audio-frontend"};
  stubborn.lifecycle.stop_wait_budget = 50ms;
  stubborn.lifecycle.kill_wait_budget = 500ms;
  config.processes = {std::move(owner), std::move(stubborn)};

  Rk3576Profile profile(std::move(config));
  CHECK(profile.start().ok());
  CHECK(profile.stop().ok());
  CHECK(profile.status().state == Rk3576ProfileState::kIdle);
  CHECK(!std::filesystem::exists(adapter_path + ".lock"));
  CHECK(!std::filesystem::exists(adapter_path + ".sock"));
}

// 保护不变量：非法拓扑在 fork 前被拒绝，包括重复音频拥有者、拥有者依赖别人、适配器节点
// 未声明依赖拥有者以及环形依赖。
void TestValidationRejectsInvalidTopology() {
  const Rk3576ProfileConfig empty;
  CHECK(!nexweave::app::validate_rk3576_profile_config(empty).ok());

  TempDirectory temp;
  const std::string adapter_path = temp.path() + "/audio-frontend";
  Rk3576ProfileConfig duplicate_owner;
  duplicate_owner.processes = {
      MakeNode("owner-a", adapter_path, "hold", true, false),
      MakeNode("owner-b", adapter_path, "hold", true, false),
  };
  CHECK(!nexweave::app::validate_rk3576_profile_config(duplicate_owner).ok());

  Rk3576ProfileConfig owner_depends;
  Rk3576ProcessSpec owner = MakeNode("audio-frontend", adapter_path, "hold", true, false);
  owner.start_after = {"other"};
  owner_depends.processes = {std::move(owner), MakeNode("other", adapter_path, "hold", false, false)};
  CHECK(!nexweave::app::validate_rk3576_profile_config(owner_depends).ok());

  Rk3576ProfileConfig adapter_without_owner;
  Rk3576ProcessSpec adapter = MakeNode("session", adapter_path, "hold", false, true);
  adapter_without_owner.processes = {std::move(adapter)};
  CHECK(!nexweave::app::validate_rk3576_profile_config(adapter_without_owner).ok());

  Rk3576ProfileConfig cycle;
  Rk3576ProcessSpec first = MakeNode("first", adapter_path, "hold", false, true);
  first.start_after = {"second"};
  Rk3576ProcessSpec second = MakeNode("second", adapter_path, "hold", false, true);
  second.start_after = {"first"};
  cycle.processes = {std::move(first), std::move(second)};
  CHECK(!nexweave::app::validate_rk3576_profile_config(cycle).ok());
}

}  // namespace

int main() {
  TestStartReadyStopAndAudioOwnership();
  TestRepeatedStartStopRestartAndCleanup();
  TestPartialStartFailureRollsBack();
  TestStartTimeoutRollsBack();
  TestCrashAfterReadyStillStops();
  TestStopEscalatesToKill();
  TestValidationRejectsInvalidTopology();
  return 0;
}

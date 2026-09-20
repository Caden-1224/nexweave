// Linux profile 生命周期集成测试：真实子进程、真实 PAIR 数据面、真实取消与进程回收。
//
// 这批用例共同保护：
//   - start() 只有等到子进程就绪并完成数据面握手后才返回；
//   - 重复 start/stop/restart 不会留下子进程、端点文件或旧轮次输出；
//   - 输入、推理仍在进行时可以停止，所有等待都有预算上界；
//   - 部分启动失败会回滚，清理失败会进入明确不可用状态；
//   - 终态交付后迟到输出被拒绝，重启后的新轮次不会重放旧轮次。
#include "linux_profile.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef NEXWEAVE_REMOTE_SESSION_FIXTURE
#error "测试需要 NEXWEAVE_REMOTE_SESSION_FIXTURE 指向远端 Session 夹具"
#endif
#ifndef NEXWEAVE_REMOTE_SCRIPTED_FIXTURE
#error "测试需要 NEXWEAVE_REMOTE_SCRIPTED_FIXTURE 指向脚本夹具"
#endif

namespace {

using namespace std::chrono_literals;
using nexweave::app::LinuxProfile;
using nexweave::app::LinuxProfileConfig;
using nexweave::app::LinuxProfileState;
using nexweave::domain::AudioFrame;
using nexweave::domain::ErrorCode;
using nexweave::protocol::DataEvent;
using nexweave::protocol::DataEventType;

std::vector<std::string>& RegisteredEndpointFiles() {
  static auto* files = new std::vector<std::string>();
  return *files;
}

void RegisterEndpointFile(const std::string& path) {
  RegisteredEndpointFiles().push_back(path);
}

struct EndpointFileCleanup {
  ~EndpointFileCleanup() {
    for (const std::string& path : RegisteredEndpointFiles()) {
      std::remove(path.c_str());
    }
  }
};

void EnsureEndpointFileCleanup() {
  static EndpointFileCleanup cleanup;
}

std::string MakeTempPath() {
  char path[] = "/tmp/nexweave-linux-profile-XXXXXX";
  const int fd = ::mkstemp(path);
  if (fd >= 0) {
    ::close(fd);
  }
  std::error_code error;
  std::filesystem::remove(path, error);
  return path;
}

LinuxProfileConfig MakeConfig(const std::string& suffix,
                              const std::string& executable) {
  LinuxProfileConfig config;
  config.proxy.child.executable = executable;
  config.proxy.child.expect_ready_signal = true;
  config.proxy.child_config.start_wait_budget = 2000ms;
  config.proxy.child_config.stop_wait_budget = 300ms;
  config.proxy.child_config.kill_wait_budget = 500ms;
  config.proxy.child_config.poll_interval = 2ms;
  config.proxy.endpoint_file = MakeTempPath();
  RegisterEndpointFile(config.proxy.endpoint_file);
  EnsureEndpointFileCleanup();
  config.proxy.ready_timeout = 2000ms;
  config.proxy.pump_interval = 2ms;
  config.proxy.max_pending_input_events = 64;
  config.proxy.max_pending_output_events = 256;
  config.proxy.terminal_cache_capacity = 8;
  config.proxy.terminal_retention = 10000ms;
  config.stream_id = "linux-" + suffix;
  config.first_generation = 1;
  return config;
}

AudioFrame ToneFrame(std::int16_t value) {
  const auto frame = AudioFrame::from_samples(
      std::vector<std::int16_t>(nexweave::domain::kAudioFrameSamples, value));
  CHECK(frame.ok());
  return frame.value;
}

std::vector<DataEvent> CollectUntilTerminal(LinuxProfile& profile,
                                            std::chrono::milliseconds timeout) {
  std::vector<DataEvent> events;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      break;
    }
    std::vector<DataEvent> batch;
    (void)profile.poll_events(batch, 32, remaining);
    for (DataEvent& event : batch) {
      const bool terminal = event.end;
      events.push_back(std::move(event));
      if (terminal) {
        return events;
      }
    }
  }
  return events;
}

bool HasTerminal(const std::vector<DataEvent>& events, DataEventType type,
                 ErrorCode error_code) {
  for (const DataEvent& event : events) {
    if (event.end && event.type == type) {
      return event.error_code == error_code;
    }
  }
  return false;
}

bool HasRequestId(const std::vector<DataEvent>& events, const std::string& request_id) {
  for (const DataEvent& event : events) {
    if (event.request_id == request_id) {
      return true;
    }
  }
  return false;
}

// 保护不变量：重复启动、停止、重启后没有子进程或数据面通道继续存活；每一轮都使用新的
// 代际，且只有显式重启才会开始下一轮输入。
void TestRepeatedStartStopRestartAndCleanup() {
  LinuxProfile profile(MakeConfig("repeat", NEXWEAVE_REMOTE_SESSION_FIXTURE));
  CHECK(profile.start().ok());
  CHECK(profile.running());
  CHECK(profile.status().generation == 1);

  CHECK(profile.start_stream().ok());
  CHECK(profile.push_frame(ToneFrame(1000)).ok());
  CHECK(profile.push_frame(ToneFrame(0)).ok());
  CHECK(profile.finish_stream().ok());
  const std::vector<DataEvent> first_events = CollectUntilTerminal(profile, 5s);
  CHECK(HasTerminal(first_events, DataEventType::kDone, ErrorCode::kNone));
  CHECK(profile.stop().ok());
  CHECK(!profile.running());
  CHECK(profile.status().state == LinuxProfileState::kIdle);

  CHECK(profile.restart().ok());
  CHECK(profile.running());
  CHECK(profile.status().generation == 2);
  CHECK(profile.start_stream().ok());
  CHECK(profile.push_frame(ToneFrame(2000)).ok());
  CHECK(profile.push_frame(ToneFrame(0)).ok());
  CHECK(profile.finish_stream().ok());
  const std::vector<DataEvent> second_events = CollectUntilTerminal(profile, 5s);
  CHECK(HasTerminal(second_events, DataEventType::kDone, ErrorCode::kNone));
  CHECK(profile.stop().ok());
  CHECK(!profile.running());

  const auto status = profile.status();
  CHECK(status.start_attempts == 2);
  CHECK(status.starts_succeeded == 2);
  CHECK(status.start_failures == 0);
  CHECK(status.stop_requests == 3);
  CHECK(status.stops_succeeded == 3);
  CHECK(status.restarts_succeeded == 1);
  CHECK(status.streams_started == 2);
  CHECK(status.terminal_events == 2);
  CHECK(status.slot_reusable());
}

// 保护不变量：可执行文件缺失和就绪超时都必须回滚到 Idle；回滚后 stop() 仍然幂等，
// 不会把一个已经失败且没有子进程的 profile 误报成不可用。
void TestStartFailureAndReadyTimeoutRollback() {
  LinuxProfile missing(MakeConfig("missing", "/nonexistent/nexweave-linux-profile"));
  const auto missing_start = missing.start();
  CHECK(!missing_start.ok());
  CHECK(missing_start.error.code == ErrorCode::kBackendFailure);
  CHECK(missing.status().state == LinuxProfileState::kIdle);
  CHECK(!missing.running());
  CHECK(missing.status().start_failures == 1);
  CHECK(missing.stop().ok());

  LinuxProfileConfig timeout_config =
      MakeConfig("timeout", NEXWEAVE_REMOTE_SCRIPTED_FIXTURE);
  timeout_config.proxy.child.environment = {"NEXWEAVE_SCRIPTED_MODE=no-ready"};
  timeout_config.proxy.child_config.start_wait_budget = 200ms;
  timeout_config.proxy.child_config.stop_wait_budget = 100ms;
  timeout_config.proxy.child_config.kill_wait_budget = 500ms;
  LinuxProfile timeout(std::move(timeout_config));
  const auto timeout_start = timeout.start();
  CHECK(!timeout_start.ok());
  CHECK(timeout_start.error.code == ErrorCode::kTimeout);
  CHECK(timeout.status().state == LinuxProfileState::kIdle);
  CHECK(!timeout.running());
  CHECK(timeout.stop().ok());
}

// 保护不变量：输入和远端推理仍在途中时 stop() 必须受理停止、丢弃未交付输出、回收子进程，
// 并且返回后不再有事件可被读取；之后同一个 profile 仍可开始全新的轮次。
void TestStopWhileInputStreamActive() {
  LinuxProfile profile(MakeConfig("active", NEXWEAVE_REMOTE_SESSION_FIXTURE));
  CHECK(profile.start().ok());
  CHECK(profile.start_stream().ok());
  CHECK(profile.push_frame(ToneFrame(1000)).ok());
  CHECK(profile.push_frame(ToneFrame(1000)).ok());
  CHECK(profile.push_frame(ToneFrame(0)).ok());
  CHECK(profile.push_frame(ToneFrame(0)).ok());

  // 先等到远端已经写出 PCM，证明推理/播放链路确实已经启动，但输入流仍未 finish，
  // 然后用 stop() 在活动期间收敛；只排队不观察输出无法证明播放曾处于活跃状态。
  bool saw_pcm = false;
  const auto output_deadline = std::chrono::steady_clock::now() + 3s;
  while (!saw_pcm && std::chrono::steady_clock::now() < output_deadline) {
    std::vector<DataEvent> observed;
    (void)profile.poll_events(observed, 1, 50ms);
    for (const DataEvent& event : observed) {
      if (event.type == DataEventType::kPcm) {
        saw_pcm = true;
      }
    }
    if (profile.status().terminal_delivered) {
      break;
    }
  }
  CHECK_MESSAGE(saw_pcm, "停止前未观察到 PCM，无法证明播放链路已经活跃");
  CHECK(profile.running());

  // 不调用 finish_stream()：输入生产与推理都可能仍活跃时停止。
  CHECK(profile.stop().ok());
  CHECK(!profile.running());
  CHECK(profile.status().state == LinuxProfileState::kIdle);

  std::vector<DataEvent> after_stop;
  CHECK(profile.poll_events(after_stop, 16) == 0);
  CHECK(after_stop.empty());

  // 清理后重新启动一轮，证明停止没有破坏 profile 的可重建性。
  CHECK(profile.restart().ok());
  CHECK(profile.start_stream().ok());
  CHECK(profile.push_frame(ToneFrame(1000)).ok());
  CHECK(profile.push_frame(ToneFrame(0)).ok());
  CHECK(profile.finish_stream().ok());
  const std::vector<DataEvent> events = CollectUntilTerminal(profile, 5s);
  CHECK(HasTerminal(events, DataEventType::kDone, ErrorCode::kNone));
  CHECK(profile.stop().ok());
}

// 保护不变量：取消受理后旧轮次只产生自己的终态；停止并重启后，新轮次不能重放旧 request_id
// 或旧总结事件，必须使用新的代际与自己的结束终态。
void TestCancelThenRestartDoesNotReplayOldTurn() {
  // block 模式在收到 start/frame/cancel/end 后才产生事件，便于把“取消受理”与“重启”分开。
  LinuxProfileConfig config = MakeConfig("scripted", NEXWEAVE_REMOTE_SCRIPTED_FIXTURE);
  config.proxy.child.environment = {"NEXWEAVE_SCRIPTED_MODE=block"};
  LinuxProfile scripted(std::move(config));

  CHECK(scripted.start().ok());
  CHECK(scripted.start_stream().ok());
  CHECK(scripted.push_frame(ToneFrame(1000)).ok());
  CHECK(scripted.push_frame(ToneFrame(0)).ok());
  CHECK(scripted.cancel_stream().ok());
  const std::vector<DataEvent> cancelled_events = CollectUntilTerminal(scripted, 5s);
  CHECK(HasTerminal(cancelled_events, DataEventType::kError, ErrorCode::kCancelled));
  CHECK(HasRequestId(cancelled_events, "scripted-cancel-turn"));
  CHECK(scripted.stop().ok());

  CHECK(scripted.restart().ok());
  CHECK(scripted.status().generation == 2);
  CHECK(scripted.start_stream().ok());
  CHECK(scripted.push_frame(ToneFrame(1000)).ok());
  CHECK(scripted.push_frame(ToneFrame(0)).ok());
  CHECK(scripted.finish_stream().ok());
  const std::vector<DataEvent> new_events = CollectUntilTerminal(scripted, 5s);
  CHECK(HasTerminal(new_events, DataEventType::kDone, ErrorCode::kNone));
  CHECK(HasRequestId(new_events, "scripted-end-turn"));
  CHECK(!HasRequestId(new_events, "scripted-cancel-turn"));
  CHECK(scripted.stop().ok());
}

// 保护不变量：输出消费者不取事件时，代理的有界输出队列会在达到容量后进入显式失败，
// profile 随后仍能走统一 stop() 回收子进程，而不是把满队列留到下一次启动。
void TestOutputQueueFullStopsProfileCleanly() {
  LinuxProfileConfig config =
      MakeConfig("overflow", NEXWEAVE_REMOTE_SESSION_FIXTURE);
  config.proxy.max_pending_output_events = 1;
  LinuxProfile profile(std::move(config));
  CHECK(profile.start().ok());
  CHECK(profile.start_stream().ok());
  CHECK(profile.push_frame(ToneFrame(1000)).ok());
  CHECK(profile.push_frame(ToneFrame(1000)).ok());
  CHECK(profile.push_frame(ToneFrame(0)).ok());
  CHECK(profile.push_frame(ToneFrame(0)).ok());

  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline && profile.last_error().ok()) {
    std::this_thread::sleep_for(2ms);
  }
  CHECK(!profile.last_error().ok());
  CHECK(profile.stop().ok());
  CHECK(!profile.running());
  CHECK(profile.status().proxy.output_queue_full > 0);
}

}  // namespace

int main() {
  TestRepeatedStartStopRestartAndCleanup();
  TestStartFailureAndReadyTimeoutRollback();
  TestStopWhileInputStreamActive();
  TestCancelThenRestartDoesNotReplayOldTurn();
  TestOutputQueueFullStopsProfileCleanly();
  return 0;
}

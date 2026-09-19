// 多进程 Session 集成测试：真实子进程、真实 SessionApp、真实 ZeroMQ 数据面。
//
// 这批用例共同保护：
//   - 输入帧持续上行时，远端在输入结束前就能完成一轮并交付文本/PCM/终态；
//   - 取消由数据面独立送达，不等待输入结束，也不留下子进程；
//   - 子进程启动失败返回结构化错误；重复 start/stop 后没有进程或通道残留。
#include "remote_session_proxy.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef NEXWEAVE_REMOTE_SCRIPTED_FIXTURE
#error "测试需要 NEXWEAVE_REMOTE_SCRIPTED_FIXTURE 指向脚本夹具"
#endif

namespace {

using nexweave::domain::AudioFrame;
using nexweave::domain::ErrorCode;
using nexweave::protocol::DataEvent;
using nexweave::protocol::DataEventType;
using nexweave::transport::RemoteSessionProxy;
using nexweave::transport::RemoteSessionProxyConfig;

// 测试进程退出时统一删除本进程登记过的端点文件。端点路径由调用方拥有，产品代码只读；
// 因此清理责任留在测试进程，避免重复运行后把 /tmp 里的夹具文件误当成产品资源残留。
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

std::string EndpointFile(const std::string& suffix) {
  return "/tmp/nexweave-remote-session-" + std::to_string(::getpid()) + "-" + suffix +
         ".endpoint";
}

AudioFrame ToneFrame(std::int16_t value) {
  const auto frame = AudioFrame::from_samples(
      std::vector<std::int16_t>(nexweave::domain::kAudioFrameSamples, value));
  CHECK(frame.ok());
  return frame.value;
}

RemoteSessionProxyConfig ProxyConfig(const std::string& suffix,
                                     const std::string& executable =
                                         NEXWEAVE_REMOTE_SESSION_FIXTURE) {
  RemoteSessionProxyConfig config;
  config.child.executable = executable;
  config.child.expect_ready_signal = true;
  config.child_config.start_wait_budget = std::chrono::milliseconds(5000);
  config.endpoint_file = EndpointFile(suffix);
  RegisterEndpointFile(config.endpoint_file);
  EnsureEndpointFileCleanup();
  config.ready_timeout = std::chrono::milliseconds(5000);
  config.pump_interval = std::chrono::milliseconds(5);
  config.max_pending_input_events = 64;
  config.max_pending_output_events = 256;
  return config;
}

std::vector<DataEvent> CollectUntilTerminal(RemoteSessionProxy& proxy,
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
    proxy.receive_events(batch, 32, remaining);
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

void AssertHasTerminal(const std::vector<DataEvent>& events, DataEventType type,
                       ErrorCode error_code) {
  bool found = false;
  for (const DataEvent& event : events) {
    if (event.end && event.type == type) {
      found = true;
      CHECK(event.error_code == error_code);
    }
  }
  CHECK_MESSAGE(found, "缺少期望终态事件");
}

void TestNormalInputBeforeEnd() {
  RemoteSessionProxy proxy(ProxyConfig("normal"));
  CHECK(proxy.start().ok());

  CHECK(proxy.queue_start_stream("remote-session", 1).ok());
  CHECK(proxy.queue_frame(ToneFrame(1000)).ok());
  CHECK(proxy.queue_frame(ToneFrame(1000)).ok());
  CHECK(proxy.queue_frame(ToneFrame(0)).ok());
  CHECK(proxy.queue_frame(ToneFrame(0)).ok());

  const std::vector<DataEvent> events = CollectUntilTerminal(proxy, std::chrono::seconds(5));
  bool has_text = false;
  bool has_pcm = false;
  bool has_done = false;
  std::string terminal_request_id;
  std::uint64_t terminal_generation = 0;
  for (const DataEvent& event : events) {
    if (event.type == DataEventType::kFinal && !event.text.empty()) {
      has_text = true;
    }
    if (event.type == DataEventType::kPcm) {
      has_pcm = true;
      CHECK(event.pcm.size() == nexweave::domain::kAudioFrameBytes);
    }
    if (event.type == DataEventType::kDone && event.end) {
      has_done = true;
      CHECK(!event.request_id.empty());
      CHECK(!event.session_id.empty());
      CHECK(event.generation >= 1);
      terminal_request_id = event.request_id;
      terminal_generation = event.generation;
    }
  }
  CHECK(has_text);
  CHECK(has_pcm);
  CHECK(has_done);
  const auto cached_terminal =
      proxy.query_terminal(terminal_request_id, terminal_generation);
  CHECK(cached_terminal.ok());
  CHECK(cached_terminal.value->end);
  const auto proxy_stats = proxy.stats();
  CHECK(proxy_stats.input_queue_capacity == 64);
  CHECK(proxy_stats.output_queue_capacity == 256);
  CHECK(proxy_stats.terminal_cache_capacity == 8);
  CHECK(proxy_stats.output_queue_peak <= 256);
  CHECK(proxy_stats.terminal_cache_peak_entries >= 1);

  // 输入尚未结束时已经完成第一轮，说明音频是持续上行且远端后端在流结束前就开始处理。
  CHECK(proxy.queue_end_stream(0).ok());
  CHECK(proxy.stop().ok());
  CHECK(!proxy.running());
}

void TestCancelBeforeTurn() {
  RemoteSessionProxy proxy(ProxyConfig("cancel"));
  CHECK(proxy.start().ok());
  CHECK(proxy.queue_start_stream("remote-session", 1).ok());
  CHECK(proxy.queue_frame(ToneFrame(1000)).ok());
  CHECK(proxy.queue_cancel_stream().ok());

  const std::vector<DataEvent> events = CollectUntilTerminal(proxy, std::chrono::seconds(5));
  AssertHasTerminal(events, DataEventType::kError, ErrorCode::kCancelled);
  CHECK(proxy.stop().ok());
  CHECK(!proxy.running());
}

void TestOutputQueueOverflow() {
  RemoteSessionProxyConfig config = ProxyConfig("overflow");
  config.max_pending_output_events = 1;
  RemoteSessionProxy proxy(config);
  CHECK(proxy.start().ok());

  std::vector<DataEvent> none;
  CHECK(proxy.receive_events(none, 1, std::chrono::milliseconds(0)) == 0);

  CHECK(proxy.queue_start_stream("remote-session", 1).ok());
  CHECK(proxy.queue_frame(ToneFrame(1000)).ok());
  CHECK(proxy.queue_frame(ToneFrame(1000)).ok());
  CHECK(proxy.queue_frame(ToneFrame(0)).ok());
  CHECK(proxy.queue_frame(ToneFrame(0)).ok());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline && proxy.last_error().ok()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(!proxy.last_error().ok());
  CHECK(proxy.last_error().code == ErrorCode::kBackendFailure);
  // 输出队列满只应阻塞数据交付，不应让控制取消本身排在一个满的数据队列后面。
  CHECK(proxy.queue_cancel_stream().ok());
  CHECK(proxy.stop().ok());
}

void TestStartFailureAndRepeatLifecycle() {
  RemoteSessionProxy bad(ProxyConfig("bad", "/nonexistent/nexweave-remote-session"));
  const auto failed = bad.start();
  CHECK(failed.error.code == ErrorCode::kBackendFailure);
  CHECK(!bad.running());

  RemoteSessionProxy proxy(ProxyConfig("repeat"));
  CHECK(proxy.start().ok());
  CHECK(proxy.stop().ok());
  CHECK(!proxy.running());

  CHECK(proxy.start().ok());
  CHECK(proxy.running());
  CHECK(proxy.stop().ok());
  CHECK(!proxy.running());
}

// 保护不变量：输入队列已满时取消事件不能丢失。fq-block 夹具在 ready 后故意不读输入，
// 使代理输入队列在一个事件内达到上限；取消必须仍能越过数据队列送达对端，并得到唯一
// kCancelled 终态。
void TestCancelWhenInputQueueFull() {
  RemoteSessionProxyConfig config =
      ProxyConfig("full-cancel", NEXWEAVE_REMOTE_SCRIPTED_FIXTURE);
  config.max_pending_input_events = 1;
  config.data.send_high_water_mark = 1;
  config.data.send_timeout = std::chrono::milliseconds(2000);
  config.child.environment = {
      "NEXWEAVE_SCRIPTED_MODE=block",
      "NEXWEAVE_SCRIPTED_BLOCK_MS=500",
  };

  RemoteSessionProxy proxy(config);
  CHECK(proxy.start().ok());
  CHECK(proxy.queue_start_stream("remote-session", 1).ok());

  bool input_queue_full = false;
  for (int attempt = 0; attempt < 512; ++attempt) {
    const auto queued = proxy.queue_frame(ToneFrame(static_cast<std::int16_t>(attempt % 100 + 1)));
    if (!queued.ok()) {
      CHECK(queued.error.code == ErrorCode::kBackendFailure);
      input_queue_full = true;
      break;
    }
  }
  CHECK_MESSAGE(input_queue_full, "未观察到输入事件队列满，夹具前置条件不成立");

  // 旧实现把取消事件塞进同一个有界队列，此时会返回 kBackendFailure 并让取消丢失。
  CHECK(proxy.queue_cancel_stream().ok());

  const std::vector<DataEvent> events =
      CollectUntilTerminal(proxy, std::chrono::seconds(5));
  AssertHasTerminal(events, DataEventType::kError, ErrorCode::kCancelled);
  CHECK(proxy.stop().ok());
  CHECK(!proxy.running());
}

// 保护不变量：旧代际或重复终态被传输层拒绝时，代理只应丢弃并记账，不能把它升级成
// kBackendFailure 并杀死仍在服务当前请求的子进程。
void TestStaleOutputEventsDoNotStopProxy() {
  RemoteSessionProxyConfig config =
      ProxyConfig("stale", NEXWEAVE_REMOTE_SCRIPTED_FIXTURE);
  config.child.environment = {"NEXWEAVE_SCRIPTED_MODE=stale"};
  RemoteSessionProxy proxy(config);
  CHECK(proxy.start().ok());

  const std::vector<DataEvent> events =
      CollectUntilTerminal(proxy, std::chrono::seconds(5));
  AssertHasTerminal(events, DataEventType::kDone, ErrorCode::kNone);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline &&
         proxy.stats().stale_output_events_filtered == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(proxy.stats().stale_output_events_filtered >= 1);
  CHECK(proxy.last_error().ok());
  CHECK(proxy.stop().ok());
  CHECK(!proxy.running());
}

}  // namespace

int main() {
  TestNormalInputBeforeEnd();
  TestCancelBeforeTurn();
  TestOutputQueueOverflow();
  TestStartFailureAndRepeatLifecycle();
  TestCancelWhenInputQueueFull();
  TestStaleOutputEventsDoNotStopProxy();
  return 0;
}

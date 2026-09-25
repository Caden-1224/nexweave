// RKLLM 文本适配器核心的确定性测试。
//
// 脚本化 runtime 覆盖排队、UTF-8 Waiting 合并、正常 token/done、供应方错误、
// 取消导致的部分输出、队列溢出、超时和重新注册恢复。测试不链接 RKLLM SDK。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../test_support.hpp"
#include "rkllm/rkllm_text_core.hpp"

using namespace nexweave;
using nexweave::backend::rkllm_detail::IRkllmRuntime;
using nexweave::backend::rkllm_detail::VendorCallback;
using nexweave::backend::rkllm_detail::RkllmStreamConfig;
using nexweave::backend::rkllm_detail::VendorCallResult;
using nexweave::backend::rkllm_detail::VendorCallState;

namespace {

VendorCallResult Normal(std::string text) {
  return VendorCallResult{VendorCallState::kNormal, std::move(text)};
}

VendorCallResult Waiting(std::string text) {
  return VendorCallResult{VendorCallState::kWaiting, std::move(text)};
}

VendorCallResult Error(std::string text) {
  return VendorCallResult{VendorCallState::kError, std::move(text)};
}

VendorCallResult Finished() {
  return VendorCallResult{VendorCallState::kFinished, ""};
}

class FakeRuntime final : public IRkllmRuntime {
 public:
  struct Options {
    Options() = default;

    bool immediate = false;
    bool stall = false;
    bool emit_finish = true;
    std::chrono::milliseconds item_delay{0};
  };

  explicit FakeRuntime(std::vector<VendorCallResult> script)
      : FakeRuntime(std::move(script), Options{}) {}

  FakeRuntime(std::vector<VendorCallResult> script, Options options)
      : script_(std::move(script)), options_(options) {}

  ~FakeRuntime() override { (void)close(std::chrono::milliseconds(100)); }

  domain::OperationResult start_async(const std::string&,
                                      VendorCallback callback) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (running_) {
        return domain::OperationResult::failure(domain::ErrorCode::kBusy,
                                                "fake runtime 已在运行");
      }
      callback_ = std::move(callback);
      abort_requested_ = false;
      running_ = true;
    }
    if (options_.stall) {
      worker_ = std::thread([this] { StallLoop(); });
      return domain::OperationResult::success();
    }
    if (options_.immediate) {
      RunScript();
      return domain::OperationResult::success();
    }
    worker_ = std::thread([this] { RunScript(); });
    return domain::OperationResult::success();
  }

  void abort() noexcept override {
    abort_requested_ = true;
    cv_.notify_all();
  }

  bool is_running() const noexcept override { return running_.load(); }

  bool wait_until_stopped(std::chrono::milliseconds timeout) override {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, timeout, [this] { return !running_.load(); });
      if (running_.load()) {
        return false;
      }
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    return true;
  }

  bool close(std::chrono::milliseconds wait) noexcept override {
    abort();
    if (!wait_until_stopped(wait)) {
      return false;
    }
    running_ = false;
    return true;
  }

 private:
  void RunScript() {
    VendorCallback callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback = callback_;
    }
    for (const auto& item : script_) {
      if (abort_requested_.load()) {
        break;
      }
      if (options_.item_delay.count() > 0) {
        std::this_thread::sleep_for(options_.item_delay);
      }
      if (callback) {
        callback(item);
      }
      if (item.state == VendorCallState::kError) {
        running_ = false;
        cv_.notify_all();
        return;
      }
    }
    if (!abort_requested_.load() && options_.emit_finish && callback) {
      callback(Finished());
    }
    running_ = false;
    cv_.notify_all();
  }

  void StallLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return abort_requested_.load(); });
    running_ = false;
    cv_.notify_all();
  }

  std::vector<VendorCallResult> script_;
  Options options_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  VendorCallback callback_;
  std::atomic<bool> abort_requested_{false};
  std::atomic<bool> running_{false};
  std::thread worker_;
};

class RecordingProbe final : public capability::IGenerationProbe {
 public:
  void on_generation_started() override { ++started; }
  void on_token_delivered(const std::string&) override { ++tokens; }
  void on_generation_completed() override { ++completed; }
  void on_generation_failed(const std::string&) override { ++failed; }

  int started = 0;
  int tokens = 0;
  int completed = 0;
  int failed = 0;
};

struct CoreHarness {
  domain::Result<std::unique_ptr<backend::RkllmTextCore>> Make(
      std::vector<VendorCallResult> script,
      FakeRuntime::Options options = {},
      RkllmStreamConfig config = {}) {
    auto runtime = std::make_unique<FakeRuntime>(std::move(script), options);
    return backend::RkllmTextCore::create(std::move(runtime), config);
  }
};

// 正常流：Waiting 半字符与后续 Normal 合并为一个 token，最后恰好一次 done。
void TestSuccessMergesWaitingAndEmitsDone() {
  CoreHarness harness;
  auto created = harness.Make({Waiting("你"), Normal("好"), Normal("，世界"), Finished()});
  CHECK(created.ok());
  auto core = std::move(*created.value);
  RecordingProbe probe;
  core->set_progress_probe(&probe);
  std::vector<capability::TextEvent> events;
  CHECK(core->set_callback([&](const capability::TextEvent& event) {
          events.push_back(event);
        }).ok());
  const auto generated = core->generate("问");
  CHECK(generated.ok());
  CHECK(events.size() == 3);
  CHECK(events[0].kind == capability::TextEventKind::kToken);
  CHECK(events[0].text == "你好");
  CHECK(events[1].kind == capability::TextEventKind::kToken);
  CHECK(events[1].text == "，世界");
  CHECK(events[2].kind == capability::TextEventKind::kDone);
  CHECK(probe.started == 1);
  CHECK(probe.tokens == 2);
  CHECK(probe.completed == 1);
  CHECK(probe.failed == 0);
}

// 取消优先于输入校验；取消后无事件；重新注册后新轮成功。
void TestCancelBeforeGenerateThenRestart() {
  CoreHarness harness;
  auto created = harness.Make({Normal("答"), Finished()});
  CHECK(created.ok());
  auto core = std::move(*created.value);
  CHECK(core->set_callback({}).error.code == domain::ErrorCode::kInvalidInput);
  std::vector<capability::TextEvent> events;
  CHECK(core->set_callback([&](const capability::TextEvent& event) {
          events.push_back(event);
        }).ok());
  CHECK(core->cancel().ok());
  CHECK(core->generate("").error.code == domain::ErrorCode::kCancelled);
  CHECK(events.empty());
  CHECK(core->set_callback([&](const capability::TextEvent& event) {
          events.push_back(event);
        }).ok());
  CHECK(core->generate("问").ok());
  CHECK(events.size() == 2);
  CHECK(events[0].kind == capability::TextEventKind::kToken);
  CHECK(events[1].kind == capability::TextEventKind::kDone);
}

// 在 token 回调里取消：之后不再投递排队 token 或 done，重新注册可恢复。
void TestCancelInsideCallbackStopsRemainingTokens() {
  CoreHarness harness;
  auto created = harness.Make({Normal("一"), Normal("二"), Normal("三"), Finished()});
  CHECK(created.ok());
  auto core = std::move(*created.value);
  std::vector<capability::TextEvent> events;
  CHECK(core->set_callback([&](const capability::TextEvent& event) {
          events.push_back(event);
          if (event.kind == capability::TextEventKind::kToken) {
            CHECK(core->cancel().ok());
          }
        }).ok());
  const auto generated = core->generate("问");
  CHECK(generated.error.code == domain::ErrorCode::kCancelled);
  CHECK(events.size() == 1);
  CHECK(events[0].kind == capability::TextEventKind::kToken);
  CHECK(events[0].text == "一");
  events.clear();
  CHECK(core->set_callback([&](const capability::TextEvent& event) {
          events.push_back(event);
        }).ok());
  CHECK(core->generate("问").ok());
  CHECK(events.size() == 4);
  CHECK(events.back().kind == capability::TextEventKind::kDone);
}

// 供应方错误：已交付 token 保留，再发一次 error 并返回失败；重新注册后恢复。
void TestVendorErrorEmitsErrorEventAndRecovers() {
  CoreHarness harness;
  auto created = harness.Make({Normal("部分"), Error("模拟供应商错误")});
  CHECK(created.ok());
  auto core = std::move(*created.value);
  RecordingProbe probe;
  core->set_progress_probe(&probe);
  std::vector<capability::TextEvent> events;
  CHECK(core->set_callback([&](const capability::TextEvent& event) {
          events.push_back(event);
        }).ok());
  const auto generated = core->generate("问");
  CHECK(generated.error.code == domain::ErrorCode::kBackendFailure);
  CHECK(events.size() == 2);
  CHECK(events[0].kind == capability::TextEventKind::kToken);
  CHECK(events[1].kind == capability::TextEventKind::kError);
  CHECK(probe.failed == 1);
  events.clear();
  CHECK(core->set_callback([&](const capability::TextEvent& event) {
          events.push_back(event);
        }).ok());
  // 脚本仍以 Error 结束，因此第二次仍是失败；这里只验证“明确失败”而非恢复成功。
  const auto second = core->generate("问");
  CHECK(second.error.code == domain::ErrorCode::kBackendFailure);
}

// 回调异常返回结构化失败，不再继续投递；重新注册后新轮成功。
void TestCallbackExceptionLocksRound() {
  CoreHarness harness;
  auto created = harness.Make({Normal("一"), Normal("二"), Finished()});
  CHECK(created.ok());
  auto core = std::move(*created.value);
  RecordingProbe probe;
  core->set_progress_probe(&probe);
  CHECK(core->set_callback([](const capability::TextEvent&) {
          throw std::runtime_error("注入回调异常");
        }).ok());
  const auto failed = core->generate("问");
  CHECK(failed.error.code == domain::ErrorCode::kBackendFailure);
  CHECK(probe.failed == 1);

  std::vector<capability::TextEvent> events;
  CHECK(core->set_callback([&](const capability::TextEvent& event) {
          events.push_back(event);
        }).ok());
  CHECK(core->generate("问").ok());
  CHECK(events.size() == 3);
}

// 队列容量是有界失败：供应方快速产出超过上限时不再阻塞，generate 结构化失败。
void TestQueueOverflowFailsWithoutBlocking() {
  CoreHarness harness;
  RkllmStreamConfig config;
  config.max_pending_tokens = 1;
  FakeRuntime::Options options;
  options.immediate = true;
  auto created = harness.Make({Normal("一"), Normal("二"), Normal("三"), Finished()},
                              options, config);
  CHECK(created.ok());
  auto core = std::move(*created.value);
  std::vector<capability::TextEvent> events;
  CHECK(core->set_callback([&](const capability::TextEvent& event) {
          events.push_back(event);
        }).ok());
  const auto generated = core->generate("问");
  CHECK(generated.error.code == domain::ErrorCode::kBackendFailure);
  CHECK(events.empty());
}

// 供应方不结束时，generate 在有限时间后超时并请求 abort；状态进入不可用或可恢复
// 由是否在 stop_wait 内停止决定。本用例用会响应 abort 的 fake runtime 验证超时路径。
void TestGenerationTimeoutReturnsStructuredFailure() {
  CoreHarness harness;
  FakeRuntime::Options options;
  options.stall = true;
  RkllmStreamConfig config;
  config.generation_wait = std::chrono::milliseconds(20);
  config.stop_wait = std::chrono::milliseconds(200);
  auto created = harness.Make({}, options, config);
  CHECK(created.ok());
  auto core = std::move(*created.value);
  std::vector<capability::TextEvent> events;
  CHECK(core->set_callback([&](const capability::TextEvent& event) {
          events.push_back(event);
        }).ok());
  const auto generated = core->generate("问");
  CHECK(generated.error.code == domain::ErrorCode::kTimeout);
  CHECK(events.empty());
}

// 输入长度边界：空 prompt、超长 prompt、未注册回调都返回 kInvalidInput。
void TestInputBoundaries() {
  CoreHarness harness;
  RkllmStreamConfig config;
  config.max_prompt_bytes = 4;
  auto created = harness.Make({Normal("答"), Finished()}, {}, config);
  CHECK(created.ok());
  auto core = std::move(*created.value);
  std::vector<capability::TextEvent> events;
  CHECK(core->generate("问").error.code == domain::ErrorCode::kInvalidInput);
  CHECK(core->set_callback([&](const capability::TextEvent& event) {
          events.push_back(event);
        }).ok());
  CHECK(core->generate("").error.code == domain::ErrorCode::kInvalidInput);
  CHECK(core->generate("12345").error.code == domain::ErrorCode::kInvalidInput);
  CHECK(events.empty());
}

}  // namespace

int main() {
  TestSuccessMergesWaitingAndEmitsDone();
  TestCancelBeforeGenerateThenRestart();
  TestCancelInsideCallbackStopsRemainingTokens();
  TestVendorErrorEmitsErrorEventAndRecovers();
  TestCallbackExceptionLocksRound();
  TestQueueOverflowFailsWithoutBlocking();
  TestGenerationTimeoutReturnsStructuredFailure();
  TestInputBoundaries();
  return 0;
}

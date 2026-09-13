// Fake 流式 LLM 的单元夹具：验收确定性 token 流、取消语义与进度观测的抑制规则。
//
// 保护的不变量（每条在断言旁注明）：
//   1. 事件形态：非空 prompt 产生 token* 后恰好一次 done，顺序固定；空 prompt 与未注册
//      回调都返回 kInvalidInput，不产生任何事件。
//   2. 取消：cancel 后新生成返回 kCancelled 且不产生事件；重新注册回调后可以开始新一轮。
//   3. 回调抛出按失败收敛：异常不穿过后端，本轮自此锁定为取消/失败态。
//   4. 进度观测的抑制是单向的：观察者一旦报告失败，后端不再报告任何后续事件——包括失败
//      本身。少这一道守卫会出现“后续轮次全程被抑制、却单独收到一条失败”的自相矛盾证据。
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include "../test_support.hpp"
#include "fake_llm.hpp"
#include "generation_probe.hpp"

using namespace nexweave;
using nexweave::domain::ErrorCode;

namespace {

// 采集事件序列，断言“有哪些事件、按什么顺序”。
std::vector<capability::TextEvent> Collect(backend::FakeLlm& llm, const std::string& prompt) {
  std::vector<capability::TextEvent> events;
  CHECK(
      llm.set_callback([&](const capability::TextEvent& event) { events.push_back(event); }).ok());
  const auto result = llm.generate(prompt);
  CHECK(result.ok() || result.error.code != ErrorCode::kNone);
  return events;
}

// 锁存型进度探针：首次失败后一直报告“已失败”，与接口文档给出的实现方式一致。
// 它同时记录收到过哪些回调，用来核对抑制规则。
class LatchingProbe final : public capability::IGenerationObserver {
 public:
  void on_generation_started() override {
    events.push_back("started");
  }

  void on_token_delivered(const std::string& /*token*/) override {
    events.push_back("token");
  }

  void on_generation_completed() override {
    events.push_back("completed");
  }

  void on_generation_failed(const std::string& /*message*/) override {
    events.push_back("failed");
    failed_ = true;
  }

  bool failed() const noexcept override {
    return failed_;
  }

  std::vector<std::string> events;

 private:
  bool failed_ = false;
};

void TestTokenStreamOrderAndShape() {
  backend::FakeLlm llm({"你好", "，世界"});
  const auto events = Collect(llm, "问候");
  CHECK(events.size() == 3);
  // 顺序固定：token 逐个交付，最后恰好一次 done。
  CHECK(events.at(0).kind == capability::TextEventKind::kToken);
  CHECK(events.at(0).text == "你好");
  CHECK(events.at(1).kind == capability::TextEventKind::kToken);
  CHECK(events.at(1).text == "，世界");
  CHECK(events.at(2).kind == capability::TextEventKind::kDone);
}

void TestInvalidPromptAndMissingCallbackProduceNoEvents() {
  backend::FakeLlm llm({"答"});
  // 未注册回调：拒绝而不是产生事件。
  const auto missing = llm.generate("问");
  CHECK(missing.error.code == ErrorCode::kInvalidInput);
  CHECK(!missing.ok());

  std::vector<capability::TextEvent> events;
  CHECK(
      llm.set_callback([&](const capability::TextEvent& event) { events.push_back(event); }).ok());
  // 空 prompt：同样拒绝，且一条事件都不产生。
  const auto empty = llm.generate("");
  CHECK(empty.error.code == ErrorCode::kInvalidInput);
  CHECK(events.empty());
}

void TestCancelLocksRoundUntilCallbackReRegistered() {
  backend::FakeLlm llm({"答"});
  std::vector<capability::TextEvent> events;
  CHECK(
      llm.set_callback([&](const capability::TextEvent& event) { events.push_back(event); }).ok());

  // 取消之后：不再产出事件，且新一轮生成被明确拒绝，而不是悄悄从头开始。
  CHECK(llm.cancel().ok());
  const auto after_cancel = llm.generate("问");
  CHECK(after_cancel.error.code == ErrorCode::kCancelled);
  CHECK(events.empty());

  // 重新注册回调即开启新一轮：取消是可以恢复的轮次边界，不是永久失效。
  events.clear();
  CHECK(
      llm.set_callback([&](const capability::TextEvent& event) { events.push_back(event); }).ok());
  CHECK(llm.generate("问").ok());
  CHECK(!events.empty());
}

void TestThrowingCallbackIsReportedAsCancelled() {
  backend::FakeLlm llm({"答"});
  CHECK(
      llm.set_callback([](const capability::TextEvent&) { throw std::runtime_error("注入"); }).ok());
  // 异常原样向上传播（后端已先封锁本轮），因此这里显式接住它：调用方看到的是异常，
  // 而本轮的状态已经在后端里锁定为取消。
  bool threw = false;
  try {
    (void)llm.generate("问");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(llm.generate("问").error.code == ErrorCode::kCancelled);
}

void TestObserverFailureSuppressesEveryLaterReport() {
  backend::FakeLlm llm({"答"});
  LatchingProbe probe;
  llm.set_observer(&probe);

  // 第一轮：进入回调的异常让本轮以失败收敛，观察者据此置位。
  CHECK(
      llm.set_callback([](const capability::TextEvent&) { throw std::runtime_error("注入"); }).ok());
  // 回调抛出的异常由后端原样向上传播（它已先封锁本轮），因此这里显式接住。
  bool first_threw = false;
  try {
    (void)llm.generate("问");
  } catch (const std::runtime_error&) {
    first_threw = true;
  }
  CHECK(first_threw);
  CHECK(probe.failed());
  const std::size_t after_first = probe.events.size();
  CHECK(after_first >= 1);
  CHECK(probe.events.back() == "failed");

  // 第二轮：观察者仍处于“已失败”状态。让这一轮同样以失败收敛（回调再抛一次），用来验证
  // 失败报告本身也被抑制——这正是缺少守卫时唯一会漏出去的报告，也是最自相矛盾的一条：
  // 观察者会收到一个它从未见过对应开始的失败。
  CHECK(
      llm.set_callback([](const capability::TextEvent&) { throw std::runtime_error("注入"); }).ok());
  bool second_threw = false;
  try {
    (void)llm.generate("问");
  } catch (const std::runtime_error&) {
    second_threw = true;
  }
  CHECK(second_threw);
  CHECK(probe.events.size() == after_first);
}

}  // namespace

int main() {
  try {
    TestTokenStreamOrderAndShape();
    TestInvalidPromptAndMissingCallbackProduceNoEvents();
    TestCancelLocksRoundUntilCallbackReRegistered();
    TestThrowingCallbackIsReportedAsCancelled();
    TestObserverFailureSuppressesEveryLaterReport();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Fake 流式 LLM 用例未通过: %s\n", error.what());
    return 1;
  }
  return 0;
}

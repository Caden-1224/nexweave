// 固定 PCM 通过公共能力入口产生可复制的文本事件；无设备、线程或时间依赖。
#include <stdexcept>
#include <string>
#include <vector>

#include "../test_support.hpp"
#include "fake_asr.hpp"

using namespace nexweave;
namespace {

// 三个合法帧必须产生 partial、partial、final、done；partial 为累计假设，
// final 使用完整夹具，done 不重复携带文本。回调副本由接收方保存。
void TestOrderedEvents() {
  backend::FakeAsr fake({"打开", "打开灯"});
  capability::IAsr& asr = fake;
  std::vector<capability::TextEvent> events;
  CHECK(asr.set_callback([&](const capability::TextEvent& event) {
             events.push_back(event);
           }).ok());
  const auto frame = domain::AudioFrame::from_samples(std::vector<std::int16_t>(320, 7));
  CHECK(frame.ok());
  CHECK(asr.feed(frame.value, false).ok());
  CHECK(asr.feed(frame.value, false).ok());
  CHECK(asr.feed(frame.value, true).ok());
  CHECK(events.size() == 4);
  CHECK(events[0].kind == capability::TextEventKind::kPartial);
  CHECK(events[0].text == "打开");
  CHECK(events[1].kind == capability::TextEventKind::kPartial);
  CHECK(events[1].text == "打开灯");
  CHECK(events[2].kind == capability::TextEventKind::kFinal);
  CHECK(events[2].text == "打开灯");
  CHECK(events[3].kind == capability::TextEventKind::kDone);
  CHECK(events[3].text.empty());
  for (const auto& event : events) {
    CHECK(event.error.ok());
  }
}


// 非法注册和非法帧不能产生事件或推进累计文本；结束标志不豁免帧校验。
// 中途注册空回调失败后，原轮次仍能继续；从未送帧时保持静默。
void TestInvalidInputDoesNotConsume() {
  backend::FakeAsr fake({"打开", "打开灯"});
  capability::IAsr& asr = fake;
  std::vector<capability::TextEvent> events;
  const auto record = [&](const capability::TextEvent& event) { events.push_back(event); };
  const auto frame = domain::AudioFrame::from_samples(std::vector<std::int16_t>(320, 7)).value;
  CHECK(asr.set_callback({}).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(asr.feed(frame, true).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(asr.set_callback(record).ok());
  CHECK(events.empty());
  CHECK(asr.feed(domain::AudioFrame{}, true).error.code == domain::ErrorCode::kInvalidInput);
  auto invalid = frame;
  invalid.sample_rate_hz = 8000;
  CHECK(asr.feed(invalid, false).error.code == domain::ErrorCode::kInvalidInput);
  invalid = frame;
  invalid.channels = 2;
  CHECK(asr.feed(invalid, false).error.code == domain::ErrorCode::kInvalidInput);
  invalid = frame;
  invalid.format = domain::AudioSampleFormat::kUnknown;
  CHECK(asr.feed(invalid, false).error.code == domain::ErrorCode::kInvalidInput);
  invalid = frame;
  invalid.samples.pop_back();
  CHECK(asr.feed(invalid, true).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(events.empty());
  CHECK(asr.feed(frame, false).ok());
  CHECK(events.size() == 1 && events[0].text == "打开");
  CHECK(asr.set_callback({}).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(asr.feed(frame, false).ok());
  CHECK(events.size() == 2 && events[1].text == "打开灯");

  backend::FakeAsr empty({});
  backend::FakeAsr empty_item({"打开", ""});
  CHECK(empty.set_callback(record).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(empty_item.set_callback(record).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(empty.feed(frame, true).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(empty_item.feed(frame, true).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(events.size() == 2);
}

// 取消前交付的副本仍归接收方，取消后不得再发 partial/final/done。
// 两次取消以及非法注册都不能解封；合法注册清游标，新轮次必须从首项开始。
void TestCancelAndRestart() {
  backend::FakeAsr fake({"打开", "打开灯"});
  capability::IAsr& asr = fake;
  std::vector<capability::TextEvent> old_events;
  std::vector<capability::TextEvent> new_events;
  const auto frame = domain::AudioFrame::from_samples(std::vector<std::int16_t>(320, 7)).value;
  CHECK(asr.set_callback([&](const capability::TextEvent& event) {
             old_events.push_back(event);
           }).ok());
  CHECK(asr.feed(frame, false).ok());
  CHECK(asr.cancel().ok());
  CHECK(asr.cancel().ok());
  CHECK(asr.feed(frame, false).error.code == domain::ErrorCode::kCancelled);
  CHECK(asr.feed(frame, true).error.code == domain::ErrorCode::kCancelled);
  CHECK(asr.feed(domain::AudioFrame{}, true).error.code == domain::ErrorCode::kCancelled);
  CHECK(asr.set_callback({}).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(asr.feed(frame, true).error.code == domain::ErrorCode::kCancelled);
  CHECK(old_events.size() == 1);
  CHECK(asr.set_callback([&](const capability::TextEvent& event) {
             new_events.push_back(event);
           }).ok());
  CHECK(asr.feed(frame, false).ok());
  CHECK(asr.feed(frame, true).ok());
  CHECK(new_events.size() == 3);
  CHECK(new_events[0].kind == capability::TextEventKind::kPartial);
  CHECK(new_events[0].text == "打开");
  CHECK(new_events[1].kind == capability::TextEventKind::kFinal);
  CHECK(new_events[1].text == "打开灯");
  CHECK(new_events[2].kind == capability::TextEventKind::kDone);
  CHECK(old_events.size() == 1);

  // 尚未注册/尚未送帧也允许取消，不虚构一个终态事件。
  backend::FakeAsr fresh({"灯"});
  CHECK(fresh.cancel().ok());
  CHECK(fresh.feed(frame, true).error.code == domain::ErrorCode::kCancelled);
}

// 回调异常可能意味着接收方已保存 final，但未返回；不得继续 done 或重放本轮。
// 以 final 回调抛出作为最小复现，验证异常透传、取消封锁和显式注册恢复。
void TestCallbackFailureClosesRound() {
  backend::FakeAsr fake({"打开", "打开灯"});
  capability::IAsr& asr = fake;
  const auto frame = domain::AudioFrame::from_samples(std::vector<std::int16_t>(320, 7)).value;
  std::vector<capability::TextEvent> events;
  CHECK(asr.set_callback([&](const capability::TextEvent& event) {
             events.push_back(event);
             throw std::runtime_error("接收方失败");
           }).ok());
  bool threw = false;
  try {
    (void)asr.feed(frame, true);
  } catch (const std::runtime_error& error) {
    threw = std::string(error.what()) == "接收方失败";
  }
  CHECK(threw);
  CHECK(events.size() == 1 && events[0].kind == capability::TextEventKind::kFinal);
  CHECK(asr.feed(frame, true).error.code == domain::ErrorCode::kCancelled);
  CHECK(events.size() == 1);
  CHECK(asr.set_callback([&](const capability::TextEvent& event) {
             events.push_back(event);
           }).ok());
  CHECK(asr.feed(frame, false).ok());
  CHECK(events.size() == 2 && events[1].text == "打开");
}

// 重复运行比较每个公开事件的类型、文本与错误，而非只比较数量。
// 夹具耗尽后 partial 保持最终假设；连续结束帧表示连续单帧轮次，不是重复旧结果。
void TestReplayAndBoundaries() {
  backend::FakeAsr fake({"打开", "打开灯"});
  capability::IAsr& asr = fake;
  std::vector<capability::TextEvent> events;
  const auto record = [&](const capability::TextEvent& event) { events.push_back(event); };
  CHECK(asr.set_callback(record).ok());
  const auto frame = domain::AudioFrame::from_samples(std::vector<std::int16_t>(320, 7)).value;
  for (int round = 0; round < 2; ++round) {
    CHECK(asr.feed(frame, false).ok());
    CHECK(asr.feed(frame, false).ok());
    CHECK(asr.feed(frame, false).ok());
    CHECK(asr.feed(frame, true).ok());
  }
  CHECK(events.size() == 10);
  CHECK(events[0].text == "打开");
  CHECK(events[2].kind == capability::TextEventKind::kPartial);
  CHECK(events[2].text == "打开灯");
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(events[i].kind == events[i + 5].kind);
    CHECK(events[i].text == events[i + 5].text);
    CHECK(events[i].error.code == events[i + 5].error.code);
    CHECK(events[i].error.message == events[i + 5].error.message);
  }
  events.clear();
  CHECK(asr.feed(frame, true).ok());
  CHECK(asr.feed(frame, true).ok());
  CHECK(events.size() == 4);
  for (std::size_t i = 0; i < events.size(); i += 2) {
    CHECK(events[i].kind == capability::TextEventKind::kFinal);
    CHECK(events[i].text == "打开灯");
    CHECK(events[i + 1].kind == capability::TextEventKind::kDone);
    CHECK(events[i + 1].text.empty());
  }

  // 活动轮次重新注册是显式丢弃未完成假设的边界，不生成旧 final/done。
  events.clear();
  CHECK(asr.feed(frame, false).ok());
  CHECK(asr.set_callback(record).ok());
  CHECK(asr.feed(frame, false).ok());
  CHECK(events.size() == 2 && events[0].text == "打开" && events[1].text == "打开");

  // 单项脚本不会下溢，源容器修改不影响对象拥有的夹具副本。
  std::vector<std::string> script{"灯"};
  backend::FakeAsr single(script);
  script[0] = "外部修改";
  events.clear();
  CHECK(single.set_callback(record).ok());
  CHECK(single.feed(frame, false).ok());
  CHECK(single.feed(frame, false).ok());
  CHECK(single.feed(frame, true).ok());
  CHECK(events.size() == 4);
  CHECK(events[0].text == "灯" && events[1].text == "灯" && events[2].text == "灯");
  CHECK(events[3].kind == capability::TextEventKind::kDone);
}
}  // namespace
int main() {
  TestOrderedEvents();
  TestInvalidInputDoesNotConsume();
  TestCancelAndRestart();
  TestCallbackFailureClosesRound();
  TestReplayAndBoundaries();
}
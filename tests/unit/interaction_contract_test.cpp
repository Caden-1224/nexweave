#include "../test_support.hpp"

#include <cstdint>
#include <vector>

#include "interaction_contract.hpp"

using namespace nexweave;
namespace {

// 成功不变量：任意长度样本只在生产者侧补零，领域帧仍严格为 320 样本，valid_samples
// 保留原始有效长度；0、1、319、320、321 样本均不能被静默截断。
void TestPaddingBoundaries() {
  for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{319}, std::size_t{320}, std::size_t{321}}) {
    const std::vector<std::int16_t> samples(count, 7);
    const auto result = runtime::pad_audio_samples(samples);
    CHECK(result.ok());
    if (count == 0) {
      CHECK(result.value->empty());
      continue;
    }
    const std::size_t expected_frames = (count + 319) / 320;
    CHECK(result.value->size() == expected_frames);
    CHECK(result.value->front().frame.samples.size() == 320);
    CHECK(result.value->front().valid_samples == std::min(count, std::size_t{320}));
    CHECK(result.value->back().valid_samples == (count % 320 == 0 ? 320 : count % 320));
  }
}

// 播放可早于生成完成；三个局部 done 不互相替代，只有最后的播放完成才产生唯一成功终态。
void TestOverlapAndTerminal() {
  runtime::InteractionContractFixture fixture;
  CHECK(fixture.start_stream("mic-1").ok());
  CHECK(fixture.begin_generation().ok());
  const auto frame = domain::AudioFrame::from_samples(std::vector<std::int16_t>(320, 1));
  runtime::InputStreamEvent input;
  input.stream_id = "mic-1";
  input.generation = 1;
  input.sequence = 0;
  input.kind = runtime::InputEventKind::kFrame;
  input.frame = frame.value;
  input.valid_samples = 320;
  CHECK(fixture.push_frame(input).ok());
  CHECK(fixture.start_playback(1).ok());
  CHECK(!fixture.terminal());
  CHECK(fixture.mark_generation_done(1).ok());
  CHECK(fixture.mark_synthesis_done(1).ok());
  CHECK(fixture.mark_playback_done(1).ok());
  CHECK(fixture.terminal());
  CHECK(fixture.last_marker() == runtime::ActivityMarker::kTerminalSucceeded);
}

// 取消先封锁旧输出，再记录执行退出和播放清理；旧 generation 的回调不能恢复终态。
void TestCancellationAndOrdering() {
  runtime::InteractionContractFixture fixture;
  CHECK(fixture.start_stream("mic-1").ok());
  CHECK(fixture.begin_generation().ok());
  CHECK(fixture.cancel(1).ok());
  CHECK(!fixture.mark_generation_done(1).ok());
  CHECK(fixture.last_marker() == runtime::ActivityMarker::kTerminalCancelled);
  const auto trace = fixture.trace();
  CHECK(trace.size() == 6);
  CHECK(trace[2] == runtime::ActivityMarker::kOldOutputBlocked);
  CHECK(trace[4] == runtime::ActivityMarker::kPlaybackCleared);
}

// 非法事件不能推进 sequence；乱序、重复、短帧和未知版本都保持错误。
// 新 generation 不改变常驻 stream 的身份，但旧 generation 输入必须被拒绝。
void TestInputValidation() {
  runtime::InteractionContractFixture fixture;
  CHECK(fixture.start_stream("mic-1").ok());
  runtime::InputStreamEvent invalid;
  invalid.stream_id = "mic-1";
  invalid.kind = runtime::InputEventKind::kFrame;
  invalid.valid_samples = 1;
  CHECK(!fixture.push_frame(invalid).ok());
  CHECK(fixture.begin_generation().ok());
  const auto frame = domain::AudioFrame::from_samples(std::vector<std::int16_t>(320));
  runtime::InputStreamEvent event{1, "mic-1", 1, 0, runtime::InputEventKind::kFrame,
                                  frame.value, 320};
  CHECK(fixture.push_frame(event).ok());
  CHECK(!fixture.push_frame(event).ok());
  event.sequence = 1;
  event.version = 2;
  CHECK(!fixture.push_frame(event).ok());
}
}

int main() {
  TestPaddingBoundaries();
  TestOverlapAndTerminal();
  TestCancellationAndOrdering();
  TestInputValidation();
}

// Fake 音频输入输出行为测试。
//
// 测试意图：在不依赖声卡的边界上保护“固定样本按 20 ms 分帧、输出可回读、
// 取消不泄漏旧代、重复运行无残留、非法输入有明确错误”的不变量。测试只通过
// FakeAudioSource/FakeAudioSink 公共接口观察行为，不依赖内部游标或容器布局。
#include "fake_audio.hpp"

#include <cstdio>
#include <cstdint>
#include <vector>

namespace {
int failures = 0;

#define CHECK(value) \
  do { \
    if (!(value)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
      ++failures; \
    } \
  } while (false)

std::vector<std::int16_t> MakePcm(std::size_t frames) {
  std::vector<std::int16_t> pcm;
  pcm.reserve(frames * nexweave::domain::kAudioFrameSamples);
  for (std::size_t index = 0; index < frames * nexweave::domain::kAudioFrameSamples;
       ++index) {
    pcm.push_back(static_cast<std::int16_t>(index % 997));
  }
  return pcm;
}

void TestSourceFramesAndEnd() {
  const auto pcm = MakePcm(2);
  nexweave::backend::FakeAudioSource source(pcm);
  CHECK(source.frame_count() == 2);
  CHECK(source.open().ok());
  CHECK(source.open().error.code == nexweave::domain::ErrorCode::kAlreadyCompleted);

  const auto first = source.read();
  const auto second = source.read();
  CHECK(first.ok());
  CHECK(second.ok());
  CHECK(first.value->samples.front() == 0);
  CHECK(first.value->samples.back() == 319);
  CHECK(second.value->samples.front() == 320);
  CHECK(second.value->samples.back() == 639);
  CHECK(source.frames_read() == 2);

  const auto end = source.read();
  CHECK(!end.ok());
  CHECK(end.error.code == nexweave::domain::ErrorCode::kAlreadyCompleted);
  CHECK(source.close().ok());
  CHECK(source.read().error.code == nexweave::domain::ErrorCode::kDeviceFailure);
}

void TestSourceRejectsEmptyAndPartialPcm() {
  nexweave::backend::FakeAudioSource empty({});
  CHECK(empty.open().error.code == nexweave::domain::ErrorCode::kInvalidInput);

  nexweave::backend::FakeAudioSource partial(
      std::vector<std::int16_t>(nexweave::domain::kAudioFrameSamples - 1));
  CHECK(partial.open().error.code == nexweave::domain::ErrorCode::kInvalidInput);
}

void TestCancellationAndDeterministicReplay() {
  const auto pcm = MakePcm(1);
  nexweave::backend::FakeAudioSource source(pcm);
  CHECK(source.open().ok());
  CHECK(source.cancel().ok());
  CHECK(source.read().error.code == nexweave::domain::ErrorCode::kCancelled);
  CHECK(source.close().ok());
  CHECK(source.open().ok());
  const auto replay = source.read();
  CHECK(replay.ok());
  CHECK(replay.value->samples == pcm);
}

void TestSinkValidationCancellationAndNoResidue() {
  nexweave::backend::FakeAudioSink sink;
  const auto valid = nexweave::domain::AudioFrame::from_samples(
      std::vector<std::int16_t>(nexweave::domain::kAudioFrameSamples, 7));
  CHECK(!sink.write(valid.value).ok());
  CHECK(sink.open().ok());
  CHECK(sink.open().error.code == nexweave::domain::ErrorCode::kAlreadyCompleted);
  CHECK(sink.write(valid.value).ok());
  CHECK(sink.frames().size() == 1);

  auto malformed = valid.value;
  malformed.samples.pop_back();
  CHECK(sink.write(malformed).error.code == nexweave::domain::ErrorCode::kInvalidInput);

  CHECK(sink.cancel().ok());
  CHECK(sink.frames().empty());
  CHECK(sink.write(valid.value).error.code == nexweave::domain::ErrorCode::kCancelled);
  CHECK(sink.close().ok());
  CHECK(sink.open().ok());
  CHECK(sink.frames().empty());
  CHECK(sink.write(valid.value).ok());
  CHECK(sink.close().ok());
  CHECK(sink.pcm_samples() == valid.value.samples);
}
}  // namespace

int main() {
  TestSourceFramesAndEnd();
  TestSourceRejectsEmptyAndPartialPcm();
  TestCancellationAndDeterministicReplay();
  TestSinkValidationCancellationAndNoResidue();
  return failures == 0 ? 0 : 1;
}

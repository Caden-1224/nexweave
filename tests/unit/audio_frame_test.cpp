// 统一音频帧契约测试。
//
// 测试意图：验证固定音频元数据、恰好 320 个 S16_LE 样本的成功路径，
// 并覆盖采样率、声道、格式和帧长错误；这些断言保护后端之间传递的
// 音频合同不会因调用方输入错误而静默改变。
#include "audio_frame.hpp"

#include <array>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;

#define CHECK(value)                                               \
  do {                                                             \
    if (!(value)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
      ++failures;                                                  \
    }                                                              \
  } while (0)

void TestValidFrame() {
  CHECK(nexweave::domain::kAudioSampleRateHz * nexweave::domain::kAudioFrameDurationMs / 1000 ==
        nexweave::domain::kAudioFrameSamples);
  CHECK(nexweave::domain::kAudioFrameBytes == 640);

  auto samples = std::array<std::int16_t, nexweave::domain::kAudioFrameSamples>{};
  samples.front() = -123;
  samples.back() = 456;
  const auto frame = nexweave::domain::AudioFrame::from_samples(samples);
  CHECK(frame.ok());
  CHECK(frame.value.sample_rate_hz == nexweave::domain::kAudioSampleRateHz);
  CHECK(frame.value.channels == nexweave::domain::kAudioChannels);
  CHECK(frame.value.format == nexweave::domain::AudioSampleFormat::kS16LE);
  CHECK(frame.value.samples.size() == samples.size());
  CHECK(frame.value.samples.front() == samples.front());
  CHECK(frame.value.samples.back() == samples.back());
  CHECK(nexweave::domain::validate_audio_frame(frame.value).ok());
  const auto validated = nexweave::domain::validate_audio_frame(frame.value);
  CHECK(validated.value.samples == frame.value.samples);

  const std::vector<std::int16_t> vector_samples(nexweave::domain::kAudioFrameSamples, 7);
  const auto copied = nexweave::domain::AudioFrame::from_samples(vector_samples);
  CHECK(copied.ok());
  CHECK(copied.value.samples.front() == 7);
  CHECK(copied.value.samples.back() == 7);

  const auto transmitted = copied.value;
  CHECK(nexweave::domain::validate_audio_frame(transmitted).ok());
}

void TestDefaultResultIsNotSuccess() {
  // 缺失载荷/默认结果必须失败，防止未初始化的反序列化字段被误当成合法帧。
  const nexweave::domain::AudioFrameValidationResult result;
  CHECK(!result.ok());
  CHECK(result.error == nexweave::domain::AudioFrameError::kInvalidSampleCount);
  CHECK(nexweave::domain::validate_audio_frame(nexweave::domain::AudioFrame{}).error ==
        nexweave::domain::AudioFrameError::kInvalidSampleCount);
}

void TestMetadataErrors() {
  auto frame = nexweave::domain::AudioFrame::from_samples(
      std::array<std::int16_t, nexweave::domain::kAudioFrameSamples>{});

  frame.value.sample_rate_hz = 8000;
  auto result = nexweave::domain::validate_audio_frame(frame.value);
  CHECK(result.error == nexweave::domain::AudioFrameError::kInvalidSampleRate);

  frame.value.sample_rate_hz = nexweave::domain::kAudioSampleRateHz;
  frame.value.channels = 2;
  result = nexweave::domain::validate_audio_frame(frame.value);
  CHECK(result.error == nexweave::domain::AudioFrameError::kInvalidChannels);

  frame.value.channels = nexweave::domain::kAudioChannels;
  frame.value.format = nexweave::domain::AudioSampleFormat::kUnknown;
  result = nexweave::domain::validate_audio_frame(frame.value);
  CHECK(result.error == nexweave::domain::AudioFrameError::kInvalidFormat);
}

void TestSampleCountError() {
  // 运行时长度边界（少一、多一）必须统一映射到 kInvalidSampleCount，且不补齐。
  const auto short_samples = std::array<std::int16_t, 319>{};
  const auto result = nexweave::domain::AudioFrame::from_samples(short_samples);
  CHECK(!result.ok());
  CHECK(result.error == nexweave::domain::AudioFrameError::kInvalidSampleCount);

  const std::vector<std::int16_t> long_samples(nexweave::domain::kAudioFrameSamples + 1);
  const auto long_result = nexweave::domain::AudioFrame::from_samples(long_samples);
  CHECK(!long_result.ok());
  CHECK(long_result.error == nexweave::domain::AudioFrameError::kInvalidSampleCount);

  auto malformed = nexweave::domain::AudioFrame{};
  malformed.samples.resize(nexweave::domain::kAudioFrameSamples - 1);
  const auto malformed_result = nexweave::domain::validate_audio_frame(malformed);
  CHECK(malformed_result.error == nexweave::domain::AudioFrameError::kInvalidSampleCount);

  const auto valid = nexweave::domain::AudioFrame::from_samples(
      std::array<std::int16_t, nexweave::domain::kAudioFrameSamples>{});
  // 重复校验是幂等的：纯验证不改变输入，后续调用仍得到成功结果。
  const auto repeated = nexweave::domain::validate_audio_frame(valid.value);
  CHECK(repeated.ok());
}
}  // namespace

int main() {
  TestValidFrame();
  TestDefaultResultIsNotSuccess();
  TestMetadataErrors();
  TestSampleCountError();
  return failures == 0 ? 0 : 1;
}

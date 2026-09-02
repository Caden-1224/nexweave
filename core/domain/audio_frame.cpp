// audio_frame.hpp 的实现。这里仅做固定合同的复制与校验，不接触任何外部资源。

#include "audio_frame.hpp"

namespace nexweave::domain {

AudioFrameValidationResult AudioFrame::from_samples(
    const std::vector<std::int16_t>& input) {
  AudioFrameValidationResult result;
  if (input.size() != kAudioFrameSamples) {
    result.error = AudioFrameError::kInvalidSampleCount;
    return result;
  }
  result.value.samples = input;
  result.error = AudioFrameError::kNone;
  return result;
}

AudioFrameValidationResult validate_audio_frame(const AudioFrame& frame) {
  AudioFrameValidationResult result;
  if (frame.sample_rate_hz != kAudioSampleRateHz) {
    result.error = AudioFrameError::kInvalidSampleRate;
    return result;
  }
  if (frame.channels != kAudioChannels) {
    result.error = AudioFrameError::kInvalidChannels;
    return result;
  }
  if (frame.format != AudioSampleFormat::kS16LE) {
    result.error = AudioFrameError::kInvalidFormat;
    return result;
  }
  if (frame.samples.size() != kAudioFrameSamples) {
    result.error = AudioFrameError::kInvalidSampleCount;
    return result;
  }
  result.value = frame;
  result.error = AudioFrameError::kNone;
  return result;
}

}  // namespace nexweave::domain

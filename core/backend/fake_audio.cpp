#include "fake_audio.hpp"

#include <utility>

namespace nexweave::backend {
namespace {

domain::OperationResult failure(domain::ErrorCode code, const char* message) {
  return domain::OperationResult::failure(code, message);
}

}  // namespace

FakeAudioSource::FakeAudioSource(std::vector<std::int16_t> pcm_samples)
    : pcm_samples_(std::move(pcm_samples)) {}

domain::OperationResult FakeAudioSource::open() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (opened_) {
    return failure(domain::ErrorCode::kAlreadyCompleted, "音频源已经打开");
  }
  if (pcm_samples_.empty() ||
      pcm_samples_.size() % domain::kAudioFrameSamples != 0) {
    return failure(domain::ErrorCode::kInvalidInput,
                   "PCM 必须包含至少一帧且长度为 320 的整数倍");
  }
  next_sample_ = 0;
  cancelled_ = false;
  opened_ = true;
  return domain::OperationResult::success();
}

domain::Result<domain::AudioFrame> FakeAudioSource::read() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return domain::Result<domain::AudioFrame>::failure(
        domain::ErrorCode::kDeviceFailure, "音频源尚未打开");
  }
  if (cancelled_) {
    return domain::Result<domain::AudioFrame>::failure(
        domain::ErrorCode::kCancelled, "音频源已取消");
  }
  if (next_sample_ == pcm_samples_.size()) {
    return domain::Result<domain::AudioFrame>::failure(
        domain::ErrorCode::kAlreadyCompleted, "音频输入已结束");
  }

  const auto begin = pcm_samples_.begin() +
                     static_cast<std::ptrdiff_t>(next_sample_);
  const auto end = begin +
                   static_cast<std::ptrdiff_t>(domain::kAudioFrameSamples);
  const std::vector<std::int16_t> frame_samples(begin, end);
  const auto frame = domain::AudioFrame::from_samples(frame_samples);
  if (!frame.ok()) {
    return domain::Result<domain::AudioFrame>::failure(
        domain::ErrorCode::kInvalidInput, "Fake 音频帧不满足固定合同");
  }
  next_sample_ += domain::kAudioFrameSamples;
  return domain::Result<domain::AudioFrame>::success(frame.value);
}

domain::OperationResult FakeAudioSource::close() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  opened_ = false;
  cancelled_ = false;
  next_sample_ = 0;
  return domain::OperationResult::success();
}

domain::OperationResult FakeAudioSource::cancel() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return failure(domain::ErrorCode::kDeviceFailure, "音频源尚未打开");
  }
  cancelled_ = true;
  return domain::OperationResult::success();
}

std::size_t FakeAudioSource::frame_count() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return pcm_samples_.size() / domain::kAudioFrameSamples;
}

std::size_t FakeAudioSource::frames_read() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return next_sample_ / domain::kAudioFrameSamples;
}

domain::OperationResult FakeAudioSink::open() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (opened_) {
    return failure(domain::ErrorCode::kAlreadyCompleted, "音频汇已经打开");
  }
  frames_.clear();
  cancelled_ = false;
  opened_ = true;
  return domain::OperationResult::success();
}

domain::OperationResult FakeAudioSink::write(const domain::AudioFrame& frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return failure(domain::ErrorCode::kDeviceFailure, "音频汇尚未打开");
  }
  if (cancelled_) {
    return failure(domain::ErrorCode::kCancelled, "音频汇已取消");
  }
  if (!domain::validate_audio_frame(frame).ok()) {
    return failure(domain::ErrorCode::kInvalidInput,
                   "输出帧不满足固定音频合同");
  }
  frames_.push_back(frame);
  return domain::OperationResult::success();
}

domain::OperationResult FakeAudioSink::close() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  opened_ = false;
  cancelled_ = false;
  return domain::OperationResult::success();
}

domain::OperationResult FakeAudioSink::cancel() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return failure(domain::ErrorCode::kDeviceFailure, "音频汇尚未打开");
  }
  cancelled_ = true;
  frames_.clear();
  return domain::OperationResult::success();
}

std::vector<domain::AudioFrame> FakeAudioSink::frames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_;
}

std::vector<std::int16_t> FakeAudioSink::pcm_samples() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::int16_t> result;
  result.reserve(frames_.size() * domain::kAudioFrameSamples);
  for (const auto& frame : frames_) {
    result.insert(result.end(), frame.samples.begin(), frame.samples.end());
  }
  return result;
}

}  // namespace nexweave::backend

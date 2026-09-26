// 受控音频前处理器：只在测试中替换 WebRTC AEC/降噪。
//
// 它不访问设备、线程或第三方库，用脚本化的错误/状态让全双工前端可以在
// 不依赖声学条件的情况下验证固定 10 ms 窗口、参考不连续重置、处理失败收敛
// 和资源清理。测试通过公开计数与输出值观察行为；它不模拟真实回声消除效果。
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "audio_processor.hpp"

namespace nexweave::runtime {

class FakeAudioProcessor final : public IAudioProcessor {
 public:
  explicit FakeAudioProcessor(std::int16_t output_value = 0)
      : output_value_(output_value) {}

  void set_open_result(domain::OperationResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    open_result_ = std::move(result);
  }

  void set_render_error(domain::ErrorCode code, std::string message = {}) {
    std::lock_guard<std::mutex> lock(mutex_);
    render_error_ = domain::Error{code, std::move(message)};
  }

  void set_capture_error(domain::ErrorCode code, std::string message = {}) {
    std::lock_guard<std::mutex> lock(mutex_);
    capture_error_ = domain::Error{code, std::move(message)};
  }

  void set_reset_failure(domain::OperationResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    reset_result_ = std::move(result);
  }

  void set_state_after_capture(AudioProcessorState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_after_capture_ = state;
  }

  void set_output_value(std::int16_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_value_ = value;
  }

  domain::OperationResult open() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
      return domain::OperationResult::failure(domain::ErrorCode::kAlreadyCompleted,
                                             "fake 前处理器已经打开");
    }
    if (!open_result_.ok()) {
      return open_result_;
    }
    initialized_ = true;
    state_ = AudioProcessorState::kNotConverged;
    stats_.state = state_;
    stats_.initialized = true;
    return domain::OperationResult::success();
  }

  domain::OperationResult process_render(const AudioProcessorFrame& frame) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                             "fake 前处理器尚未打开");
    }
    if (!render_error_.ok()) {
      ++stats_.process_render_errors;
      state_ = AudioProcessorState::kFailed;
      stats_.state = state_;
      return domain::OperationResult::failure(render_error_.code,
                                             render_error_.message);
    }
    render_frames_.push_back(frame);
    ++stats_.render_frames;
    ++stats_.render_frames_since_reset;
    ++stats_.delay_measurements;
    stats_.fraction_poor_delays = 0.0F;
    if (state_ == AudioProcessorState::kNotConverged ||
        state_ == AudioProcessorState::kConverging) {
      state_ = AudioProcessorState::kConverging;
      stats_.state = state_;
    }
    return domain::OperationResult::success();
  }

  domain::Result<AudioProcessorFrame> process_capture(
      const AudioProcessorFrame& frame) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      return domain::Result<AudioProcessorFrame>::failure(
          domain::ErrorCode::kDeviceFailure, "fake 前处理器尚未打开");
    }
    if (!capture_error_.ok()) {
      ++stats_.process_capture_errors;
      state_ = AudioProcessorState::kFailed;
      stats_.state = state_;
      return domain::Result<AudioProcessorFrame>::failure(
          capture_error_.code, capture_error_.message);
    }
    capture_frames_.push_back(frame);
    ++stats_.capture_frames;
    ++stats_.capture_frames_since_reset;
    AudioProcessorFrame output{};
    output.fill(output_value_);
    if (state_after_capture_ != AudioProcessorState::kClosed) {
      state_ = state_after_capture_;
    }
    stats_.state = state_;
    return domain::Result<AudioProcessorFrame>::success(output);
  }

  domain::OperationResult reset(const std::string& reason) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++reset_calls_;
    last_reset_reason_ = reason;
    ++stats_.resets;
    stats_.last_reset_reason = reason;
    if (!reset_result_.ok()) {
      state_ = AudioProcessorState::kFailed;
      stats_.state = state_;
      return reset_result_;
    }
    state_ = AudioProcessorState::kNotConverged;
    stats_.state = state_;
    stats_.render_frames_since_reset = 0;
    stats_.capture_frames_since_reset = 0;
    stats_.delay_measurements = 0;
    stats_.fraction_poor_delays = -1.0F;
    return domain::OperationResult::success();
  }

  domain::OperationResult close() noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = false;
    state_ = AudioProcessorState::kClosed;
    stats_.initialized = false;
    stats_.state = state_;
    return domain::OperationResult::success();
  }

  AudioProcessorState state() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

  AudioProcessorStats stats() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

  // 测试观测接口；返回副本，调用方可以长期保留。
  std::vector<AudioProcessorFrame> render_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return render_frames_;
  }

  std::vector<AudioProcessorFrame> capture_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capture_frames_;
  }

  std::size_t reset_calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reset_calls_;
  }

  std::string last_reset_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_reset_reason_;
  }

 private:
  mutable std::mutex mutex_;
  bool initialized_ = false;
  AudioProcessorState state_ = AudioProcessorState::kClosed;
  AudioProcessorState state_after_capture_ = AudioProcessorState::kClosed;
  AudioProcessorStats stats_{};
  domain::OperationResult open_result_{};
  domain::Error render_error_{};
  domain::Error capture_error_{};
  domain::OperationResult reset_result_{};
  std::int16_t output_value_ = 0;
  std::vector<AudioProcessorFrame> render_frames_;
  std::vector<AudioProcessorFrame> capture_frames_;
  std::size_t reset_calls_ = 0;
  std::string last_reset_reason_;
};

}  // namespace nexweave::runtime

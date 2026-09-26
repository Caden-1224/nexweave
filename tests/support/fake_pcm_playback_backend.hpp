// 受控 PCM 播放后端：仅在测试中替代真实 ALSA 播放设备。
//
// 它让 AlsaAudioSink 的短写、underrun、超时、取消、断开、重连、排空和回放参考
// 路径具备确定性：测试可以排定 write 返回值，或打开 block_on_write 让 write 真正
// 阻塞并由 cancel 唤醒。对象不访问声卡、文件、socket 或线程。
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "alsa/pcm_playback_backend.hpp"

namespace nexweave::backend {

class FakePcmPlaybackBackend final : public PcmPlaybackBackend {
 public:
  explicit FakePcmPlaybackBackend(PcmPlaybackBackendConfig config = {})
      : config_(std::move(config)), format_(MakeFormat(config_)) {}

  void set_open_result(domain::OperationResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    open_result_ = std::move(result);
  }

  void set_recover_result(domain::OperationResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    recover_result_ = std::move(result);
  }

  void set_drain_result(domain::OperationResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    drain_result_ = std::move(result);
  }

  void set_auto_accept(bool value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto_accept_ = value;
  }

  void set_block_on_write(bool value) {
    std::lock_guard<std::mutex> lock(mutex_);
    block_on_write_ = value;
  }

  void set_block_on_drain(bool value) {
    std::lock_guard<std::mutex> lock(mutex_);
    block_on_drain_ = value;
  }

  void push_write_result(PcmPlaybackWriteResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    write_results_.push_back(std::move(result));
  }

  void push_status(PcmPlaybackWriteKind kind,
                   domain::ErrorCode code = domain::ErrorCode::kNone,
                   std::string message = {}) {
    push_write_result(PcmPlaybackWriteResult::Status(kind, code, std::move(message)));
  }

  void set_format(PcmPlaybackFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    format_ = format;
  }

  domain::OperationResult open() override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.open_attempts;
    if (open_result_.ok()) {
      opened_ = true;
      cancelled_ = false;
      ++stats_.open_successes;
      stats_.actual_sample_rate_hz = format_.sample_rate_hz;
      stats_.actual_channels = format_.channels;
      stats_.actual_format = format_.format;
      stats_.actual_period_frames = config_.period_frames;
      stats_.actual_buffer_frames = config_.buffer_frames;
    }
    return open_result_;
  }

  PcmPlaybackWriteResult write(const std::int32_t* interleaved_samples,
                               std::size_t frames,
                               std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    ++stats_.write_calls;
    if (!opened_) {
      return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kDeviceFailure,
                                            domain::ErrorCode::kDeviceFailure,
                                            "fake 播放后端尚未打开");
    }
    if (cancelled_) {
      return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kCancelled,
                                            domain::ErrorCode::kCancelled,
                                            "fake 播放后端已取消");
    }
    if (!write_results_.empty()) {
      PcmPlaybackWriteResult result = std::move(write_results_.front());
      write_results_.pop_front();
      NoteResultLocked(result);
      if (result.kind == PcmPlaybackWriteKind::kWritten) {
        RecordLocked(interleaved_samples, result.frames_written);
      }
      return result;
    }
    if (block_on_write_) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (true) {
        if (cancelled_) {
          return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kCancelled,
                                                domain::ErrorCode::kCancelled,
                                                "fake 播放后端已取消");
        }
        if (!write_results_.empty()) {
          PcmPlaybackWriteResult result = std::move(write_results_.front());
          write_results_.pop_front();
          NoteResultLocked(result);
          if (result.kind == PcmPlaybackWriteKind::kWritten) {
            RecordLocked(interleaved_samples, result.frames_written);
          }
          return result;
        }
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
          ++stats_.timeout_calls;
          return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kTimeout,
                                                domain::ErrorCode::kTimeout,
                                                "fake 播放写入等待超时");
        }
      }
    }
    if (auto_accept_) {
      RecordLocked(interleaved_samples, frames);
      ++stats_.written_calls;
      return PcmPlaybackWriteResult::Written(frames, 0);
    }
    ++stats_.timeout_calls;
    return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kTimeout,
                                          domain::ErrorCode::kTimeout,
                                          "fake 播放后端没有可写空间");
  }

  domain::OperationResult recover() override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.recover_attempts;
    if (cancelled_) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled,
                                              "fake 播放后端恢复期间已取消");
    }
    if (!recover_result_.ok()) {
      return recover_result_;
    }
    opened_ = true;
    ++stats_.recover_successes;
    return domain::OperationResult::success();
  }

  domain::OperationResult drain(std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    ++stats_.drain_calls;
    if (block_on_drain_) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (drain_result_.ok() && !cancelled_) {
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
          ++stats_.drain_timeouts;
          return domain::OperationResult::failure(domain::ErrorCode::kTimeout,
                                                  "fake 排空等待超时");
        }
      }
    }
    return drain_result_;
  }

  domain::OperationResult cancel() noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.cancelled_calls;
    cancelled_ = true;
    cv_.notify_all();
    return domain::OperationResult::success();
  }

  domain::OperationResult close() noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.close_calls;
    opened_ = false;
    cancelled_ = false;
    cv_.notify_all();
    return domain::OperationResult::success();
  }

  PcmPlaybackFormat current_format() const noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    return format_;
  }

  PcmPlaybackBackendStats stats() const noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

  std::vector<std::int32_t> accepted_samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return accepted_samples_;
  }

  std::size_t pending_write_results() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return write_results_.size();
  }

 private:
  static PcmPlaybackFormat MakeFormat(const PcmPlaybackBackendConfig& config) {
    PcmPlaybackFormat format;
    format.sample_rate_hz = config.sample_rate_hz;
    format.channels = config.channels;
    format.format = config.format;
    return format;
  }

  void RecordLocked(const std::int32_t* samples, std::size_t frames) {
    if (samples == nullptr || frames == 0) {
      return;
    }
    const std::size_t count = frames * format_.channels;
    accepted_samples_.insert(accepted_samples_.end(), samples, samples + count);
  }

  void NoteResultLocked(const PcmPlaybackWriteResult& result) {
    switch (result.kind) {
      case PcmPlaybackWriteKind::kWritten:
        ++stats_.written_calls;
        break;
      case PcmPlaybackWriteKind::kRecovered:
        ++stats_.recovered_calls;
        break;
      case PcmPlaybackWriteKind::kTimeout:
        ++stats_.timeout_calls;
        break;
      case PcmPlaybackWriteKind::kCancelled:
        break;
      case PcmPlaybackWriteKind::kDeviceFailure:
        ++stats_.device_failures;
        break;
      case PcmPlaybackWriteKind::kAlreadyCompleted:
        break;
    }
  }

  PcmPlaybackBackendConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  PcmPlaybackFormat format_;
  domain::OperationResult open_result_{};
  domain::OperationResult recover_result_{};
  domain::OperationResult drain_result_{};
  bool opened_ = false;
  bool cancelled_ = false;
  bool auto_accept_ = true;
  bool block_on_write_ = false;
  bool block_on_drain_ = false;
  std::deque<PcmPlaybackWriteResult> write_results_;
  std::vector<std::int32_t> accepted_samples_;
  PcmPlaybackBackendStats stats_;
};

}  // namespace nexweave::backend

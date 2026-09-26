// 受控 PCM 采集后端：仅在测试中替代真实 ALSA 设备。
//
// 它让 AlsaAudioSource 的阻塞读、超时、取消、短读、overrun、断开和重连路径具备
// 确定性：测试通过 push_read_result() 预先排定后端返回，或打开 block_on_read 让
// read 真正阻塞并由 cancel 唤醒。该对象不访问声卡、文件、socket 或线程，只使用
// mutex/condition_variable 表达等待；测试结束后由适配器 close 并释放。
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

#include "alsa/pcm_capture_backend.hpp"

namespace nexweave::backend {

class FakePcmCaptureBackend final : public PcmCaptureBackend {
 public:
  explicit FakePcmCaptureBackend(PcmCaptureBackendConfig config = {})
      : config_(std::move(config)), format_(MakeFormat(config_)) {}

  // 设置 open() 的默认返回值；默认成功。成功时把 opened_ 置为 true。
  // 单独设置会清空 push_open_result() 排定的序列，便于测试明确重置场景。
  void set_open_result(domain::OperationResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    open_result_ = std::move(result);
    open_results_.clear();
  }

  // 排定一次 open() 的返回值；队列非空时优先消费队首，队空后回到默认值。
  // 用于模拟设备断开后前几次重开失败、随后恢复的有限重试路径。
  void push_open_result(domain::OperationResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    open_results_.push_back(std::move(result));
  }

  // 设置 recover() 的返回值；默认成功。失败时由适配器映射为设备失败。
  void set_recover_result(domain::OperationResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    recover_result_ = std::move(result);
  }

  // 设置 read() 在没有排队结果时是否阻塞等待，直到超时或 cancel。默认 false，
  // 此时无排队结果会立即返回 kTimeout，便于测试“不空转”的预算路径。
  void set_block_on_read(bool block) {
    std::lock_guard<std::mutex> lock(mutex_);
    block_on_read_ = block;
  }

  // 排定一次 read 返回值。队列按 FIFO 消费；队列为空时走阻塞/超时分支。
  void push_read_result(PcmCaptureReadResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    read_results_.push_back(std::move(result));
    cv_.notify_all();
  }

  // 排定一块数据；调用方保证 samples 是 file 对应的交错样本。
  void push_data(PcmCaptureFormat format, std::vector<std::int32_t> samples) {
    PcmCaptureBlock block;
    block.format = format;
    block.interleaved_samples = std::move(samples);
    push_read_result(PcmCaptureReadResult::Data(std::move(block)));
  }

  // 排定一个状态结果，便于测试短读、恢复、断开等分支。
  void push_status(PcmCaptureReadKind kind,
                   domain::ErrorCode code = domain::ErrorCode::kNone,
                   std::string message = {}) {
    push_read_result(PcmCaptureReadResult::Status(kind, code, std::move(message)));
  }

  // 覆盖 current_format(); 用于测试 open 后实际格式与请求值不同的情况。
  void set_format(PcmCaptureFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    format_ = format;
  }

  domain::OperationResult open() override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.open_attempts;
    domain::OperationResult result = open_result_;
    if (!open_results_.empty()) {
      result = std::move(open_results_.front());
      open_results_.pop_front();
    }
    if (result.ok()) {
      opened_ = true;
      cancelled_ = false;
      ++stats_.open_successes;
      stats_.actual_sample_rate_hz = format_.sample_rate_hz;
      stats_.actual_channels = format_.channels;
      stats_.actual_format = format_.format;
      stats_.actual_period_frames = config_.period_frames;
      stats_.actual_buffer_frames = config_.buffer_frames;
    }
    return result;
  }

  PcmCaptureReadResult read(std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    ++stats_.read_calls;
    if (!opened_) {
      return PcmCaptureReadResult::Status(PcmCaptureReadKind::kDeviceFailure,
                                          domain::ErrorCode::kDeviceFailure,
                                          "fake 后端尚未打开");
    }
    if (cancelled_) {
      return PcmCaptureReadResult::Status(PcmCaptureReadKind::kCancelled,
                                          domain::ErrorCode::kCancelled,
                                          "fake 后端已取消");
    }
    if (!read_results_.empty()) {
      PcmCaptureReadResult result = std::move(read_results_.front());
      read_results_.pop_front();
      NoteResultLocked(result);
      return result;
    }
    if (!block_on_read_) {
      stats_.timeout_blocks++;
      return PcmCaptureReadResult::Status(PcmCaptureReadKind::kTimeout,
                                          domain::ErrorCode::kTimeout,
                                          "fake 后端无排队数据");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      if (cancelled_) {
        return PcmCaptureReadResult::Status(PcmCaptureReadKind::kCancelled,
                                            domain::ErrorCode::kCancelled,
                                            "fake 后端已取消");
      }
      if (!read_results_.empty()) {
        PcmCaptureReadResult result = std::move(read_results_.front());
        read_results_.pop_front();
        NoteResultLocked(result);
        return result;
      }
      if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
        stats_.timeout_blocks++;
        return PcmCaptureReadResult::Status(PcmCaptureReadKind::kTimeout,
                                            domain::ErrorCode::kTimeout,
                                            "fake 后端等待超时");
      }
    }
  }

  domain::OperationResult recover() override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.recover_attempts;
    if (cancelled_) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled,
                                              "fake 后端恢复期间已取消");
    }
    if (!recover_result_.ok()) {
      return recover_result_;
    }
    opened_ = true;
    ++stats_.recover_successes;
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.cancel_calls;
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

  PcmCaptureFormat current_format() const noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    return format_;
  }

  PcmCaptureBackendStats stats() const noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

  // 测试观测：已经入队但尚未消费的后端结果数。
  std::size_t pending_read_results() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return read_results_.size();
  }

 private:
  static PcmCaptureFormat MakeFormat(const PcmCaptureBackendConfig& config) {
    PcmCaptureFormat format;
    format.sample_rate_hz = config.sample_rate_hz;
    format.channels = config.channels;
    format.format = config.format;
    return format;
  }

  void NoteResultLocked(const PcmCaptureReadResult& result) {
    switch (result.kind) {
      case PcmCaptureReadKind::kData:
        ++stats_.data_blocks;
        if (result.block.frame_count() < config_.period_frames) {
          ++stats_.short_reads;
        }
        break;
      case PcmCaptureReadKind::kTimeout:
        ++stats_.timeout_blocks;
        break;
      case PcmCaptureReadKind::kRecovered:
        ++stats_.recovered_blocks;
        break;
      case PcmCaptureReadKind::kCancelled:
        break;
      case PcmCaptureReadKind::kDeviceFailure:
        ++stats_.device_failures;
        break;
      case PcmCaptureReadKind::kAlreadyCompleted:
        break;
    }
  }

  PcmCaptureBackendConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  PcmCaptureFormat format_;
  domain::OperationResult open_result_{};
  std::deque<domain::OperationResult> open_results_;
  domain::OperationResult recover_result_{};
  bool opened_ = false;
  bool cancelled_ = false;
  bool block_on_read_ = false;
  std::deque<PcmCaptureReadResult> read_results_;
  PcmCaptureBackendStats stats_;
};

}  // namespace nexweave::backend

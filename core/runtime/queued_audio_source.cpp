#include "queued_audio_source.hpp"

#include <utility>

namespace nexweave::runtime {
namespace {

using domain::ErrorCode;
using domain::OperationResult;

}  // namespace

domain::OperationResult validate_queued_audio_source_config(
    const QueuedAudioSourceConfig& config) {
  if (config.max_pending_frames == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "帧队列容量必须为正");
  }
  return OperationResult::success();
}

QueuedAudioSource::QueuedAudioSource(QueuedAudioSourceConfig config)
    : config_(std::move(config)) {
  if (!validate_queued_audio_source_config(config_).ok()) {
    config_ = QueuedAudioSourceConfig{};
  }
}

domain::OperationResult QueuedAudioSource::open() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (opened_ && !closed_) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted, "音频源已经打开");
  }
  // close() 之后的 open 是同一对象的新输入轮次：旧队列不保留，取消标志复位。
  // 初始 open 前的 push 是允许的启动窗口，不能被清掉，否则先到帧会被静默丢弃。
  if (closed_) {
    pending_.clear();
  }
  opened_ = true;
  closed_ = false;
  ended_ = false;
  cancelled_ = false;
  return OperationResult::success();
}

domain::Result<domain::AudioFrame> QueuedAudioSource::read() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!opened_) {
    return domain::Result<domain::AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                                       "音频源尚未打开");
  }
  condition_.wait(lock, [this] {
    return closed_ || cancelled_ || ended_ || !pending_.empty();
  });

  if (closed_) {
    return domain::Result<domain::AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                                       "音频源已经关闭");
  }
  if (cancelled_) {
    note_cancelled_read_locked();
    return domain::Result<domain::AudioFrame>::failure(ErrorCode::kCancelled,
                                                       "音频源已经取消");
  }
  if (!pending_.empty()) {
    domain::AudioFrame frame = std::move(pending_.front());
    pending_.pop_front();
    ++stats_.popped;
    return domain::Result<domain::AudioFrame>::success(std::move(frame));
  }
  // 只有 ended_ 可能从上面的谓词剩下：没有帧且不会再产出。
  return domain::Result<domain::AudioFrame>::failure(ErrorCode::kAlreadyCompleted,
                                                     "音频输入已经结束");
}

domain::OperationResult QueuedAudioSource::cancel() noexcept {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_) {
      return OperationResult::success();
    }
    cancelled_ = true;
    ended_ = false;
    pending_.clear();
  }
  condition_.notify_all();
  return OperationResult::success();
}

domain::OperationResult QueuedAudioSource::close() noexcept {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return OperationResult::success();
    }
    closed_ = true;
    opened_ = false;
    cancelled_ = false;
    ended_ = false;
    pending_.clear();
  }
  condition_.notify_all();
  return OperationResult::success();
}

domain::OperationResult QueuedAudioSource::push(domain::AudioFrame frame) {
  const auto valid = domain::validate_audio_frame(frame);
  if (!valid.ok()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "只接受合法固定音频帧");
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return OperationResult::failure(ErrorCode::kDeviceFailure, "音频源已经关闭");
    }
    if (cancelled_) {
      return OperationResult::failure(ErrorCode::kCancelled, "音频源已经取消");
    }
    if (ended_) {
      return OperationResult::failure(ErrorCode::kAlreadyCompleted, "音频输入已经结束");
    }
    if (pending_.size() >= config_.max_pending_frames) {
      ++stats_.rejected_full;
      return OperationResult::failure(ErrorCode::kBackendFailure, "音频输入队列已满");
    }
    pending_.push_back(std::move(frame));
    ++stats_.pushed;
  }
  condition_.notify_one();
  return OperationResult::success();
}

domain::OperationResult QueuedAudioSource::end_input() noexcept {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || cancelled_ || ended_) {
      return OperationResult::success();
    }
    ended_ = true;
  }
  condition_.notify_all();
  return OperationResult::success();
}

bool QueuedAudioSource::has_pending_frames() const noexcept {
  const std::lock_guard<std::mutex> lock(mutex_);
  return !pending_.empty();
}

QueuedAudioSourceStats QueuedAudioSource::stats() const noexcept {
  const std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void QueuedAudioSource::note_cancelled_read_locked() {
  ++stats_.cancelled_reads;
}

}  // namespace nexweave::runtime

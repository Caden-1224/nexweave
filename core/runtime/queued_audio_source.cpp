#include "queued_audio_source.hpp"

#include <utility>

namespace nexweave::runtime {
namespace {

using domain::ErrorCode;
using domain::OperationResult;

BoundedQueueConfig MakeQueueConfig(const QueuedAudioSourceConfig& config) {
  BoundedQueueConfig queue_config;
  queue_config.capacity = config.max_pending_frames;
  // 连续音频的中间帧不允许被最新帧覆盖；满队列必须显式失败。
  queue_config.drop_oldest_on_full = false;
  return queue_config;
}

}  // namespace

domain::OperationResult validate_queued_audio_source_config(
    const QueuedAudioSourceConfig& config) {
  if (config.max_pending_frames == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "帧队列容量必须为正");
  }
  return OperationResult::success();
}

QueuedAudioSource::QueuedAudioSource(QueuedAudioSourceConfig config)
    : queue_(std::make_unique<BoundedQueue<domain::AudioFrame>>(MakeQueueConfig(config))),
      config_(std::move(config)) {
  if (!validate_queued_audio_source_config(config_).ok()) {
    config_ = QueuedAudioSourceConfig{};
    queue_ = std::make_unique<BoundedQueue<domain::AudioFrame>>(MakeQueueConfig(config_));
  }
}

domain::OperationResult QueuedAudioSource::open() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (opened_ && !closed_) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted, "音频源已经打开");
  }
  // 取消可能先于会话线程真正打开输入：此时若 open() 只是清掉 cancelled_，会留下一个
  // 已经关闭但三个终态标志都为假的队列，read() 会把这种内部不一致报告成设备无数据。
  // 取消状态只能由 close() 清除；未打开先取消时必须让建立输入显式失败，而不是假装
  // 可以重新开始一轮。
  if (!opened_ && cancelled_) {
    return OperationResult::failure(ErrorCode::kCancelled, "音频源已经取消");
  }
  // 记录 open 之前是否已经收到自然结束。end_input() 可以早于 open() 到达：字节流
  // 已结束，但已经入队的完整帧仍按顺序有效。此时不能把 ended_ 清掉，否则 read() 会在
  // 队列关闭后看到三个终态标志都为假，从而把一次正常结束误报成设备无数据。
  const bool ended_before_open = !opened_ && ended_ && !closed_;
  // close() 之后的 open 是同一对象的新输入轮次：旧队列不保留，取消标志复位。
  // 初始 open 前的 push 是允许的启动窗口，不能被清掉，否则先到帧会被静默丢弃。
  if (closed_) {
    queue_ = std::make_unique<BoundedQueue<domain::AudioFrame>>(MakeQueueConfig(config_));
  }
  opened_ = true;
  closed_ = false;
  if (!ended_before_open) {
    ended_ = false;
  }
  cancelled_ = false;
  return OperationResult::success();
}

domain::Result<domain::AudioFrame> QueuedAudioSource::read() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
      return domain::Result<domain::AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                                         "音频源尚未打开");
    }
    if (cancelled_) {
      ++cancelled_reads_;
      return domain::Result<domain::AudioFrame>::failure(ErrorCode::kCancelled,
                                                         "音频源已经取消");
    }
    if (closed_) {
      return domain::Result<domain::AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                                         "音频源已经关闭");
    }
  }

  domain::AudioFrame frame;
  const QueuePopStatus status = queue_->pop_blocking(frame);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (status == QueuePopStatus::kItem) {
      return domain::Result<domain::AudioFrame>::success(std::move(frame));
    }
    if (cancelled_) {
      ++cancelled_reads_;
      return domain::Result<domain::AudioFrame>::failure(ErrorCode::kCancelled,
                                                         "音频源已经取消");
    }
    if (closed_) {
      return domain::Result<domain::AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                                         "音频源已经关闭");
    }
    if (ended_) {
      return domain::Result<domain::AudioFrame>::failure(ErrorCode::kAlreadyCompleted,
                                                         "音频输入已经结束");
    }
    return domain::Result<domain::AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                                       "音频源没有可读数据");
  }
}

domain::OperationResult QueuedAudioSource::cancel() noexcept {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_) {
      return OperationResult::success();
    }
    cancelled_ = true;
    ended_ = false;
    queue_->clear();
    queue_->close();
  }
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
    queue_->clear();
    queue_->close();
  }
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
    const QueuePushStatus pushed = queue_->try_push(std::move(frame));
    if (pushed == QueuePushStatus::kAccepted) {
      return OperationResult::success();
    }
    if (pushed == QueuePushStatus::kRejectedFull) {
      return OperationResult::failure(ErrorCode::kBackendFailure, "音频输入队列已满");
    }
    if (pushed == QueuePushStatus::kRejectedClosed) {
      return OperationResult::failure(ErrorCode::kDeviceFailure, "音频源已经关闭");
    }
    return OperationResult::failure(ErrorCode::kBackendFailure, "音频输入队列拒绝了本次写入");
  }
}

domain::OperationResult QueuedAudioSource::end_input() noexcept {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || cancelled_ || ended_) {
      return OperationResult::success();
    }
    ended_ = true;
    queue_->close();
  }
  return OperationResult::success();
}

bool QueuedAudioSource::has_pending_frames() const noexcept {
  const std::lock_guard<std::mutex> lock(mutex_);
  return queue_->size() > 0;
}

QueuedAudioSourceStats QueuedAudioSource::stats() const noexcept {
  const std::lock_guard<std::mutex> lock(mutex_);
  const BoundedQueueStats queue_stats = queue_->stats();
  QueuedAudioSourceStats output;
  output.pushed = queue_stats.push_accepted;
  output.popped = queue_stats.pop_items;
  output.rejected_full = queue_stats.rejected_full;
  output.cancelled_reads = cancelled_reads_;
  output.capacity_frames = queue_stats.capacity;
  output.peak_pending_frames = queue_stats.peak_size;
  output.wait_count = queue_stats.waits;
  output.wait_timeouts = queue_stats.wait_timeouts;
  return output;
}

}  // namespace nexweave::runtime

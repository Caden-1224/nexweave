#include "resident_audio_input.hpp"

#include <utility>

namespace nexweave::runtime {

ResidentAudioInput::ResidentAudioInput(capability::IAudioSource& source,
                                       ISpeechActivityDetector& detector, std::string stream_id,
                                       ResidentAudioInputConfig config)
    : source_(source),
      detector_(detector),
      stream_id_(std::move(stream_id)),
      config_(config),
      segmenter_(stream_id_, config_.segmentation) {}

domain::OperationResult ResidentAudioInput::start() {
  if (stream_id_.empty()) {
    // 没有流标识就没有归属：交付出去的段无法回指是哪条输入流，因此拒绝开始。
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "常驻输入缺少流标识");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      return domain::OperationResult::failure(domain::ErrorCode::kAlreadyCompleted,
                                             "常驻采集已经开始");
    }
    // 先占位再打开源：每个实例只允许一次成功的 start()。
    started_ = true;
  }
  const auto opened = source_.open();
  if (!opened.ok()) {
    // 打开失败回滚占位，使调用方在修正环境（设备、权限、文件）后可以重试；
    // 不回滚会让一次瞬时失败永久锁死这条输入。
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
    return opened;
  }
  // 判定器复位在打开成功之后：打开失败时保持判定器原状，避免半启动状态。
  detector_.reset();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    capturing_ = true;
    terminal_ = false;
  }
  return domain::OperationResult::success();
}

domain::Result<std::size_t> ResidentAudioInput::pump(std::size_t max_frames) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (terminal_) {
      // 终态先于“未开始”判断：已经结束或停止的输入再泵帧不是调用错误，
      // 而是“没有新帧”，返回 0 让调用方用统一条件退出泵帧循环。
      return domain::Result<std::size_t>::success(0);
    }
    if (!capturing_) {
      return domain::Result<std::size_t>::failure(domain::ErrorCode::kInvalidInput,
                                                 "常驻采集尚未开始");
    }
  }

  std::size_t read_frames = 0;
  for (std::size_t index = 0; index < max_frames; ++index) {
    // 每帧读设备之前重新检查终态：stop() 可能在上一次迭代之后到达，此时必须让
    // 本次 pump 尽快返回，而不是把剩余预算全部读完。
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (terminal_) {
        break;
      }
    }
    // 读设备期间不持锁：真实声卡的 read 可能阻塞到它自己声明的超时，
    // 持锁会让 stop() 一直等在这次读上，取消无法及时生效。
    const auto frame = source_.read();
    if (!frame.ok()) {
      return handle_source_error(frame.error, read_frames);
    }
    const auto activity = detector_.detect(*frame.value);
    if (!activity.ok()) {
      // 判定不可用绝不当作静音：把失败当静音会静默截断用户正在说的话，
      // 而且失败原因会从证据里消失。这里结束采集并原样上报。
      finalize_failure(activity.error);
      return domain::Result<std::size_t>::failure(activity.error.code, activity.error.message);
    }
    SpeechSegment produced;
    const auto step = segmenter_.push(*frame.value, *activity.value, produced);
    if (!step.ok()) {
      // 源与判定器都已保证帧合法，因此这里只可能是契约被绕过的实现错误；
      // 仍按失败收敛而不是继续推进，避免带着非法状态继续分段。
      finalize_failure(step.error);
      return domain::Result<std::size_t>::failure(step.error.code, step.error.message);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sync_segmenter_counters_locked();
      if (step.value->speech_started) {
        publish_speech_start_locked(*step.value);
      }
      if (step.value->produced_segment) {
        enqueue_segment_locked(std::move(produced));
      }
    }
    ++read_frames;
  }
  return domain::Result<std::size_t>::success(read_frames);
}

SegmentTakeResult ResidentAudioInput::take_segment() {
  std::lock_guard<std::mutex> lock(mutex_);
  return take_locked();
}

SegmentTakeResult ResidentAudioInput::wait_for_segment() {
  std::unique_lock<std::mutex> lock(mutex_);
  // 谓词式等待同时处理虚假唤醒与“终态先于进入等待”的时序：无论结束或停止发生在
  // 进入等待之前还是等待期间，都会立即返回 kEnded，因此消费方不需要重试超时。
  cv_.wait(lock, [this] { return !pending_.empty() || terminal_; });
  return take_locked();
}

SegmentTakeResult ResidentAudioInput::wait_for_segment(std::size_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  // 与非超时重载共用同一个谓词和同一个取段状态机，区别只有“等待是否有截止时间”。
  cv_.wait_until(lock, deadline, [this] { return !pending_.empty() || terminal_; });
  SegmentTakeResult result = take_locked();
  if (result.availability == SegmentAvailability::kPending) {
    // 既没有段也没有终态：说明是超时返回。用独立状态表达，避免把超时伪装成
    // “暂时没有”——阻塞入口的调用方需要区分“再等等”和“时间到了”。
    result.availability = SegmentAvailability::kTimeout;
  }
  return result;
}

domain::OperationResult ResidentAudioInput::end_input() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (terminal_) {
      // 重复结束不改任何状态，也不重复冲刷：否则同一段音频会被交付两次。
      return domain::OperationResult::failure(domain::ErrorCode::kAlreadyCompleted,
                                              "输入已经结束或已停止");
    }
  }
  finalize_natural_end();
  return domain::OperationResult::success();
}

domain::OperationResult ResidentAudioInput::stop() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (terminal_) {
      // 幂等：停止是收尾动作，重复调用不改变计数，也不丢弃任何新东西。
      return domain::OperationResult::success();
    }
  }
  // 放弃正在进行的说话：停止发生在“这句话是否说完”无法确认的时刻，
  // 把截断音频当完整一句交付会让识别结果与证据同时失真。
  const std::size_t discarded_speech_frames = segmenter_.abandon();
  bool must_close = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    record_abandon_locked(discarded_speech_frames);
    // 显式停止丢弃尚未被取走的等待段与未消费的起音通知：这批音频是否还算数
    // 已经无法确认。丢弃一律计数，因此不是静默丢帧。
    dropped_segments_ += pending_.size();
    pending_.clear();
    pending_start_.reset();
    sync_segmenter_counters_locked();
    must_close = close_capture_locked();
  }
  finish_capture(must_close);
  return domain::OperationResult::success();
}

bool ResidentAudioInput::capturing() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return capturing_;
}

domain::Error ResidentAudioInput::last_error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

std::optional<SpeechStartNotice> ResidentAudioInput::take_speech_started() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pending_start_.has_value()) {
    return std::nullopt;
  }
  std::optional<SpeechStartNotice> notice = std::move(pending_start_);
  pending_start_.reset();
  return notice;
}

const std::string& ResidentAudioInput::stream_id() const noexcept {
  return stream_id_;
}

std::uint64_t ResidentAudioInput::frames_pumped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_pumped_;
}

std::size_t ResidentAudioInput::queued_segments() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queued_segments_;
}

std::size_t ResidentAudioInput::dropped_segments() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_segments_;
}

std::size_t ResidentAudioInput::dropped_short_segments() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_short_segments_;
}

std::size_t ResidentAudioInput::abandoned_segments() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return abandoned_segments_;
}

std::size_t ResidentAudioInput::abandoned_speech_frames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return abandoned_speech_frames_;
}

std::size_t ResidentAudioInput::merged_speech_starts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return merged_speech_starts_;
}

std::size_t ResidentAudioInput::pending_segments() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_.size();
}

SegmentTakeResult ResidentAudioInput::take_locked() {
  SegmentTakeResult result;
  if (!pending_.empty()) {
    result.availability = SegmentAvailability::kAvailable;
    result.segment = std::move(pending_.front());
    pending_.pop_front();
    return result;
  }
  result.availability = terminal_ ? SegmentAvailability::kEnded : SegmentAvailability::kPending;
  return result;
}

void ResidentAudioInput::enqueue_segment_locked(SpeechSegment segment) {
  if (config_.pending_segment_capacity == 0) {
    // 上限为 0 表示不允许排队：完成的段被立即丢弃并计数，用于“只关心当前一句”的配置。
    ++dropped_segments_;
    return;
  }
  while (pending_.size() >= config_.pending_segment_capacity) {
    // 溢出策略：丢弃最旧的等待段并计数。最旧的一段是已经过期的用户意图，而最新的一段
    // 才是用户刚说的话；丢最新的一段会让设备听不见当前指令。丢弃一定有计数与可观测的
    // pending_segments() 变化，因此调用方不会把它误当成“用户从未说过”。
    pending_.pop_front();
    ++dropped_segments_;
  }
  pending_.push_back(std::move(segment));
  ++queued_segments_;
  cv_.notify_all();
}

void ResidentAudioInput::publish_speech_start_locked(const SegmenterStep& step) {
  if (pending_start_.has_value()) {
    // 上一条起音还没被会话消费：合并计数并保留最早的一条。打断只需要知道“用户已经
    // 开始说话”，归属取最早的一条——它就是本次打断的直接原因。合并而不是覆盖，
    // 是为了让“发生过两次起音”这个事实仍然可观测。
    ++merged_speech_starts_;
    return;
  }
  SpeechStartNotice notice;
  notice.stream_id = stream_id_;
  notice.segment_id = step.segment_id;
  notice.start_sequence = step.start_sequence;
  notice.speech_sequence = step.speech_sequence;
  pending_start_ = std::move(notice);
}

void ResidentAudioInput::finalize_natural_end() {
  SpeechSegment tail;
  const auto flushed = segmenter_.flush(tail);
  const bool has_tail = flushed.ok() && *flushed.value;
  bool must_close = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (has_tail) {
      enqueue_segment_locked(std::move(tail));
    }
    sync_segmenter_counters_locked();
    must_close = close_capture_locked();
  }
  finish_capture(must_close);
}

void ResidentAudioInput::finalize_failure(const domain::Error& error) {
  // 设备失败或判定失败：音频是否连续、说话是否说完都无法确认，在途说话一律放弃。
  const std::size_t discarded_speech_frames = segmenter_.abandon();
  bool must_close = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    record_abandon_locked(discarded_speech_frames);
    // 已经完成的段是完整采集到的音频，仍然有效：失败不清空等待队列，消费方可以把
    // 它们取完再看到 kEnded。只有显式 stop() 才丢弃它们——那表示调用方不再要这批音频。
    last_error_ = error;
    sync_segmenter_counters_locked();
    must_close = close_capture_locked();
  }
  finish_capture(must_close);
}

domain::Result<std::size_t> ResidentAudioInput::handle_source_error(const domain::Error& error,
                                                                   std::size_t read_frames) {
  if (error.code == domain::ErrorCode::kAlreadyCompleted) {
    // 源自然读完是唯一会冲刷未完成说话的路径：此时音频完整，说话只是没有等到静音
    // 超时。返回成功，让调用方用“读到的帧数”而不是错误码判断循环结束。
    finalize_natural_end();
    return domain::Result<std::size_t>::success(read_frames);
  }
  finalize_failure(error);
  return domain::Result<std::size_t>::failure(error.code, error.message);
}

bool ResidentAudioInput::close_capture_locked() {
  const bool must_close = capturing_;
  // 先置终态再关闭源：等待者在锁内就能看到 kEnded，不会因为设备关闭耗时而被卡住。
  capturing_ = false;
  terminal_ = true;
  return must_close;
}

void ResidentAudioInput::sync_segmenter_counters_locked() {
  // 分段器只由输入拥有者线程推进；把它的计数复制到互斥量保护下的快照字段，
  // 其它线程才能在一致的前提下读取，而不是直接读一个正在被写入的对象。
  frames_pumped_ = segmenter_.frames_fed();
  dropped_short_segments_ = segmenter_.dropped_short_segments();
}

void ResidentAudioInput::record_abandon_locked(std::size_t discarded_speech_frames) {
  if (discarded_speech_frames == 0) {
    // 没有在途人声时不算一次放弃：这两个计数统计的是“被丢弃的说话”，不是调用次数。
    return;
  }
  ++abandoned_segments_;
  abandoned_speech_frames_ += discarded_speech_frames;
}

void ResidentAudioInput::finish_capture(bool must_close) {
  close_source(must_close);
  // 唤醒放在最后一步：等待者被唤醒时看到的一定是已经置好的终态与已经入队的段。
  cv_.notify_all();
}

void ResidentAudioInput::close_source(bool must_close) {
  if (!must_close) {
    return;
  }
  // 关闭在锁外完成：真实设备关闭可能等待工作线程退出，持锁会拖住消费者。
  const auto closed = source_.close();
  if (closed.ok()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (last_error_.ok()) {
    // 关闭失败没有独立上报通道（stop 幂等且不抛异常），只在尚无更早错误时记录，
    // 保证“这条输入为什么结束”始终有据可查，同时不覆盖真正的根因。
    last_error_ = closed.error;
  }
}

}  // namespace nexweave::runtime

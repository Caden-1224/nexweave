#include "audio_frontend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nexweave::runtime {
namespace {

using domain::AudioFrame;
using domain::Error;
using domain::ErrorCode;
using domain::OperationResult;

std::int64_t NowSteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::int16_t ClampToInt16(std::int32_t value) {
  return static_cast<std::int16_t>(std::clamp(value, -32768, 32767));
}

std::int16_t NativeSampleToInt16(std::int32_t sample,
                                 backend::PlaybackSampleFormat format) {
  if (format == backend::PlaybackSampleFormat::kS16LE) {
    return ClampToInt16(sample);
  }
  // S24_LE 在 32 位容器中只使用低 24 位；右移 8 位做符号保持缩放。
  return ClampToInt16(sample >> 8);
}

// 把实际写入设备的原生交错样本变成 16 kHz 单声道 S16 时间轴。
//
// 该对象只在 audio_frontend.cpp 内部使用，由调用方的 render_mutex_ 保护。它不
// 拥有设备、线程或 socket；唯一的资源是 native_/output_ 两个有界由调用方检查的
// 队列。每个 output_ 样本都带有预计实际播放时间，采集线程据此判断某个 10 ms
// 槽应该消费真实参考还是零填充。
struct ScheduledSample {
  std::int16_t value = 0;
  std::int64_t play_at_ns = -1;
};

class RenderReferenceTimeline {
 public:
  struct NativeSample {
    std::int16_t value = 0;
    std::int64_t play_at_ns = -1;
  };

  void Reset() {
    native_.clear();
    output_.clear();
    format_known_ = false;
    format_ = backend::PcmPlaybackFormat{};
    native_begin_index_ = 0;
    next_output_position_ = 0.0;
    last_input_play_ns_ = -1;
    pending_reset_reason_.clear();
    has_pending_reset_ = false;
  }

  void MarkDiscontinuityLocked(const std::string& reason) {
    native_.clear();
    output_.clear();
    format_known_ = false;
    format_ = backend::PcmPlaybackFormat{};
    native_begin_index_ = 0;
    next_output_position_ = 0.0;
    last_input_play_ns_ = -1;
    pending_reset_reason_ = reason;
    has_pending_reset_ = true;
  }

  std::string TakePendingResetReasonLocked() {
    if (!has_pending_reset_) {
      return {};
    }
    has_pending_reset_ = false;
    return std::move(pending_reset_reason_);
  }

  // 追加一个实际成功写入设备的参考块。返回新生成并排入 output_ 的 16 kHz 样本
  // 数；格式不合法或时间信息不可用时返回 false，调用方必须按不连续处理。
  bool AppendBlockLocked(const backend::AlsaPlaybackReferenceBlock& block,
                         std::int64_t now_ns, std::size_t* produced) {
    if (produced == nullptr) {
      return false;
    }
    *produced = 0;
    if (!block.format.valid() || block.interleaved_samples.empty()) {
      return false;
    }
    if (format_known_ &&
        (block.format.sample_rate_hz != format_.sample_rate_hz ||
         block.format.channels != format_.channels ||
         block.format.format != format_.format)) {
      MarkDiscontinuityLocked("播放参考格式变化");
    }
    format_ = block.format;
    format_known_ = true;

    const std::uint32_t rate = format_.sample_rate_hz;
    const std::uint16_t channels = format_.channels;
    if (rate == 0 || channels == 0) {
      return false;
    }

    std::int64_t start_ns = block.write_done_steady_ns;
    if (start_ns < 0) {
      start_ns = now_ns;
    }
    if (block.delay_frames >= 0) {
      start_ns += static_cast<std::int64_t>(
          (static_cast<double>(block.delay_frames) * 1.0e9) /
          static_cast<double>(rate));
    }
    if (last_input_play_ns_ >= 0 && start_ns <= last_input_play_ns_) {
      start_ns = last_input_play_ns_ + 1;
    }

    const std::size_t frames =
        block.interleaved_samples.size() / static_cast<std::size_t>(channels);
    const double ns_per_frame = 1.0e9 / static_cast<double>(rate);

    for (std::size_t frame = 0; frame < frames; ++frame) {
      std::int64_t sum = 0;
      for (std::uint16_t channel = 0; channel < channels; ++channel) {
        const std::int32_t raw =
            block.interleaved_samples[frame * channels + channel];
        sum += NativeSampleToInt16(raw, format_.format);
      }
      const std::int16_t mono =
          ClampToInt16(static_cast<std::int32_t>(sum / channels));
      std::int64_t play_at_ns =
          start_ns + static_cast<std::int64_t>(static_cast<double>(frame) * ns_per_frame);
      if (last_input_play_ns_ >= 0 && play_at_ns <= last_input_play_ns_) {
        play_at_ns = last_input_play_ns_ + 1;
      }
      native_.push_back({mono, play_at_ns});
      last_input_play_ns_ = play_at_ns;
    }
    const std::size_t before = output_.size();
    ProduceOutputsLocked();
    *produced = output_.size() - before;
    return true;
  }

  // 从时间轴取出 count 个样本：预计播放时间未到的槽填零；已经到期的样本按序
  // 消费。late_exceeded 为 true 表示至少一个样本迟到超过调用方上界，调用方
  // 应在下一个 10 ms 槽重置 AEC。
  std::size_t PopReadyLocked(std::int64_t now_ns, std::int16_t* output,
                             std::size_t count, std::int64_t tolerance_ns,
                             std::int64_t max_lateness_ns, std::size_t* zero_filled,
                             std::size_t* late_samples, bool* late_exceeded) {
    if (output == nullptr || zero_filled == nullptr || late_samples == nullptr ||
        late_exceeded == nullptr) {
      return 0;
    }
    *late_exceeded = false;
    std::size_t consumed = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if (output_.empty() || output_.front().play_at_ns > now_ns + tolerance_ns) {
        output[i] = 0;
        ++(*zero_filled);
        continue;
      }
      const ScheduledSample sample = output_.front();
      output_.pop_front();
      output[i] = sample.value;
      ++consumed;
      if (sample.play_at_ns + max_lateness_ns < now_ns) {
        ++(*late_samples);
        *late_exceeded = true;
      }
    }
    return consumed;
  }

  std::size_t queued_samples() const {
    return output_.size();
  }

 private:
  void ProduceOutputsLocked() {
    if (!format_known_ || format_.sample_rate_hz == 0) {
      return;
    }
    const double native_per_output =
        static_cast<double>(format_.sample_rate_hz) /
        static_cast<double>(domain::kAudioSampleRateHz);
    while (true) {
      const auto floor_index =
          static_cast<std::int64_t>(std::floor(next_output_position_));
      while (native_begin_index_ < floor_index && !native_.empty()) {
        native_.pop_front();
        ++native_begin_index_;
      }
      if (native_.empty()) {
        break;
      }
      if (native_.size() == 1) {
        // 流式线性插值的最后一个可用样本：只有输出位置正好落在该样本上时
        // 才能用零阶保持结算，否则必须等待下一块输入，不能提前猜未来样本。
        if (next_output_position_ <=
            static_cast<double>(native_begin_index_)) {
          output_.push_back({native_[0].value, native_[0].play_at_ns});
          next_output_position_ += native_per_output;
          continue;
        }
        break;
      }
      if (native_begin_index_ > floor_index) {
        break;
      }
      const double fraction =
          next_output_position_ - static_cast<double>(floor_index);
      const double interpolated =
          (1.0 - fraction) * static_cast<double>(native_[0].value) +
          fraction * static_cast<double>(native_[1].value);
      const long rounded = std::lround(interpolated);
      const std::int16_t value =
          static_cast<std::int16_t>(std::clamp(rounded, -32768L, 32767L));
      const std::int64_t play_at_ns =
          native_[0].play_at_ns +
          static_cast<std::int64_t>(
              fraction *
              static_cast<double>(native_[1].play_at_ns - native_[0].play_at_ns));
      output_.push_back({value, play_at_ns});
      next_output_position_ += native_per_output;

      const auto next_floor =
          static_cast<std::int64_t>(std::floor(next_output_position_));
      while (native_begin_index_ < next_floor && !native_.empty()) {
        native_.pop_front();
        ++native_begin_index_;
      }
    }
  }

  bool format_known_ = false;
  backend::PcmPlaybackFormat format_{};
  std::deque<NativeSample> native_;
  std::deque<ScheduledSample> output_;
  std::int64_t native_begin_index_ = 0;
  double next_output_position_ = 0.0;
  std::int64_t last_input_play_ns_ = -1;
  std::string pending_reset_reason_;
  bool has_pending_reset_ = false;
};

}  // namespace

struct DuplexAudioFrontend::Impl {
  Impl(AudioFrontendConfig config,
       std::unique_ptr<backend::PcmCaptureBackend> capture_backend,
       std::unique_ptr<backend::PcmPlaybackBackend> playback_backend,
       std::unique_ptr<IAudioProcessor> processor)
      : config_(std::move(config)),
        capture_(),
        playback_(),
        processor_(std::move(processor)) {
    capture_.reset(new backend::AlsaAudioSource(
        config_.source, std::move(capture_backend)));
    playback_.reset(new backend::AlsaAudioSink(
        config_.sink, std::move(playback_backend)));
    playback_->set_playback_reference_callback(
        [this](const backend::AlsaPlaybackReferenceBlock& block) {
          OnPlaybackReference(block);
        });
  }

  ~Impl() {
    (void)CloseCapture();
    (void)ClosePlayback();
  }

  // 采集生命周期 -------------------------------------------------------------

  domain::OperationResult OpenCapture() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++stats_.capture_open_attempts;
      if (capture_open_) {
        return OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                        "常驻采集已经打开");
      }
      if (capture_closing_) {
        return OperationResult::failure(ErrorCode::kBusy, "常驻采集正在关闭");
      }
      capture_state_ = AudioPipelineState::kStarting;
      capture_cancelled_ = false;
      capture_terminal_ = false;
      capture_error_ = Error{};
      processed_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(render_mutex_);
      timeline_.Reset();
    }

    const auto opened = capture_->open();
    if (!opened.ok()) {
      RecordCaptureOpenFailure(opened.error);
      return opened;
    }
    const auto processor_opened = processor_->open();
    if (!processor_opened.ok()) {
      (void)capture_->close();
      RecordCaptureOpenFailure(processor_opened.error);
      return processor_opened;
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      capture_open_ = true;
      capture_state_ = AudioPipelineState::kReady;
      ++stats_.capture_open_successes;
    }

    try {
      capture_thread_ = std::thread([this]() { CaptureLoop(); });
      capture_thread_joinable_ = true;
    } catch (const std::exception& error) {
      (void)capture_->close();
      (void)processor_->close();
      std::lock_guard<std::mutex> lock(state_mutex_);
      capture_open_ = false;
      capture_state_ = AudioPipelineState::kFailed;
      capture_error_ = Error{ErrorCode::kBackendFailure,
                             std::string("采集线程创建失败: ") + error.what()};
      return OperationResult::failure(ErrorCode::kBackendFailure,
                                      capture_error_.message);
    }
    return OperationResult::success();
  }

  domain::Result<AudioFrame> ReadProcessed() {
    std::unique_lock<std::mutex> lock(state_mutex_);
    ++stats_.capture_reads;
    while (true) {
      if (capture_cancelled_) {
        return domain::Result<AudioFrame>::failure(ErrorCode::kCancelled,
                                                   "常驻采集已经取消");
      }
      if (!processed_.empty()) {
        AudioFrame frame = std::move(processed_.front());
        processed_.pop_front();
        return domain::Result<AudioFrame>::success(std::move(frame));
      }
      if (capture_terminal_) {
        if (!capture_error_.ok()) {
          return domain::Result<AudioFrame>::failure(capture_error_.code,
                                                     capture_error_.message);
        }
        return domain::Result<AudioFrame>::failure(ErrorCode::kAlreadyCompleted,
                                                   "常驻采集已经结束");
      }
      if (!capture_open_) {
        return domain::Result<AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                                   "常驻采集尚未打开");
      }
      const auto status = processed_cv_.wait_for(lock, config_.source.read_timeout);
      if (status == std::cv_status::timeout) {
        if (!processed_.empty() || capture_terminal_ || capture_cancelled_) {
          continue;
        }
        return domain::Result<AudioFrame>::failure(ErrorCode::kTimeout,
                                                   "等待处理后音频超时");
      }
    }
  }

  domain::OperationResult CancelCapture() noexcept {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!capture_open_) {
        return OperationResult::success();
      }
      if (capture_cancelled_) {
        return OperationResult::success();
      }
      capture_cancelled_ = true;
      capture_state_ = AudioPipelineState::kStopped;
      processed_.clear();
    }
    processed_cv_.notify_all();
    if (capture_) {
      (void)capture_->cancel();
    }
    return OperationResult::success();
  }

  domain::OperationResult CloseCapture() noexcept {
    bool must_join = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++stats_.capture_close_calls;
      if (!capture_open_ && !capture_thread_joinable_) {
        capture_state_ = AudioPipelineState::kStopped;
        capture_cancelled_ = false;
        capture_terminal_ = false;
        capture_error_ = Error{};
        return OperationResult::success();
      }
      capture_closing_ = true;
      capture_cancelled_ = true;
      must_join = capture_thread_joinable_;
    }
    processed_cv_.notify_all();
    if (capture_) {
      (void)capture_->cancel();
    }
    if (must_join && capture_thread_.joinable()) {
      capture_thread_.join();
    }
    (void)capture_->close();
    (void)processor_->close();
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      capture_open_ = false;
      capture_thread_joinable_ = false;
      capture_closing_ = false;
      capture_cancelled_ = false;
      capture_terminal_ = false;
      capture_state_ = AudioPipelineState::kStopped;
      capture_error_ = Error{};
      processed_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(render_mutex_);
      timeline_.Reset();
    }
    processed_cv_.notify_all();
    return OperationResult::success();
  }

  // 播放生命周期 -------------------------------------------------------------

  domain::OperationResult OpenPlayback() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++stats_.playback_open_attempts;
      if (playback_open_) {
        return OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                        "播放输出已经打开");
      }
      if (playback_closing_) {
        return OperationResult::failure(ErrorCode::kBusy, "播放输出正在关闭");
      }
      playback_state_ = AudioPipelineState::kStarting;
      playback_cancelled_ = false;
      playback_error_ = Error{};
    }
    const auto opened = playback_->open();
    if (!opened.ok()) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      playback_state_ = AudioPipelineState::kStopped;
      playback_error_ = opened.error;
      return opened;
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      playback_open_ = true;
      playback_state_ = AudioPipelineState::kReady;
      ++stats_.playback_open_successes;
      stats_.actual_playback_format = playback_->actual_playback_format();
    }
    MarkReferenceDiscontinuity("播放输出打开");
    return OperationResult::success();
  }

  domain::OperationResult WritePlayback(const AudioFrame& frame) {
    const auto validation = domain::validate_audio_frame(frame);
    if (!validation.ok()) {
      return OperationResult::failure(ErrorCode::kInvalidInput,
                                      "播放帧不符合统一音频契约");
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!playback_open_) {
        return OperationResult::failure(ErrorCode::kDeviceFailure,
                                        "播放输出尚未打开");
      }
      if (playback_cancelled_) {
        return OperationResult::failure(ErrorCode::kCancelled,
                                        "播放输出已经取消");
      }
      ++stats_.playback_writes;
    }
    const auto written = playback_->write(frame);
    if (!written.ok()) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      playback_error_ = written.error;
      playback_state_ = AudioPipelineState::kFailed;
      return written;
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      stats_.playback_frames += 1;
    }
    return OperationResult::success();
  }

  domain::OperationResult CancelPlayback() noexcept {
    bool should_cancel = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!playback_open_ || playback_cancelled_) {
        return OperationResult::success();
      }
      playback_cancelled_ = true;
      playback_state_ = AudioPipelineState::kStopped;
      should_cancel = true;
    }
    MarkReferenceDiscontinuity("播放输出取消");
    if (should_cancel && playback_) {
      return playback_->cancel();
    }
    return OperationResult::success();
  }

  domain::OperationResult ClosePlayback() noexcept {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++stats_.playback_close_calls;
      if (!playback_open_ && !playback_closing_) {
        playback_state_ = AudioPipelineState::kStopped;
        playback_cancelled_ = false;
        playback_error_ = Error{};
        return OperationResult::success();
      }
      playback_closing_ = true;
    }
    const auto closed = playback_->close();
    MarkReferenceDiscontinuity("播放输出关闭");
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      playback_open_ = false;
      playback_closing_ = false;
      playback_cancelled_ = false;
      playback_state_ = AudioPipelineState::kStopped;
      if (!closed.ok()) {
        playback_error_ = closed.error;
      }
    }
    return closed;
  }

  // 观测与状态 ---------------------------------------------------------------

  AudioPipelineState capture_state() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return capture_state_;
  }

  AudioPipelineState playback_state() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return playback_state_;
  }

  AudioProcessorState aec_state() const {
    std::lock_guard<std::mutex> lock(processor_mutex_);
    return processor_->state();
  }

  Error capture_error() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return capture_error_;
  }

  Error playback_error() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return playback_error_;
  }

  Error processor_error() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return processor_error_;
  }

  backend::PcmCaptureFormat actual_capture_format() const noexcept {
    if (!capture_) {
      return backend::PcmCaptureFormat{};
    }
    return capture_->actual_capture_format();
  }

  backend::PcmPlaybackFormat actual_playback_format() const noexcept {
    if (!playback_) {
      return backend::PcmPlaybackFormat{};
    }
    return playback_->actual_playback_format();
  }

  AudioFrontendStats stats() const {
    AudioFrontendStats snapshot;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      snapshot = stats_;
    }
    {
      std::lock_guard<std::mutex> lock(processor_mutex_);
      snapshot.processor = processor_->stats();
      snapshot.aec_state = snapshot.processor.state;
    }
    snapshot.capture_state = capture_state();
    snapshot.playback_state = playback_state();
    snapshot.actual_capture_format = actual_capture_format();
    snapshot.actual_playback_format = actual_playback_format();
    return snapshot;
  }

 private:
  void CaptureLoop() noexcept {
    while (true) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (capture_closing_ || capture_cancelled_) {
          break;
        }
      }
      const auto read = capture_->read();
      if (!read.ok()) {
        const ErrorCode code = read.error.code;
        if (code == ErrorCode::kTimeout) {
          continue;
        }
        if (code == ErrorCode::kCancelled) {
          break;
        }
        if (code == ErrorCode::kAlreadyCompleted) {
          FinishCaptureTerminal(Error{}, false);
          break;
        }
        if (code == ErrorCode::kDeviceFailure) {
          if (RecoverCapture(read.error)) {
            continue;
          }
          FinishCaptureTerminal(capture_error(), true);
          break;
        }
        FinishCaptureTerminal(read.error, true);
        break;
      }
      if (!domain::validate_audio_frame(*read.value).ok()) {
        FinishCaptureTerminal(Error{ErrorCode::kDeviceFailure,
                                    "采集帧不符合统一音频契约"},
                              true);
        break;
      }
      if (!ProcessRawFrame(*read.value)) {
        Error error = processor_error();
        if (error.ok()) {
          error = Error{ErrorCode::kBackendFailure, "音频前处理失败"};
        }
        FinishCaptureTerminal(error, true);
        break;
      }
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++stats_.capture_frames;
      }
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!capture_closing_ && !capture_terminal_) {
        capture_terminal_ = true;
        capture_state_ = AudioPipelineState::kStopped;
      }
    }
    processed_cv_.notify_all();
  }

  bool ProcessRawFrame(const AudioFrame& frame) {
    std::array<std::int16_t, domain::kAudioFrameSamples> processed{};
    for (std::size_t half = 0; half < 2; ++half) {
      const std::size_t offset = half * kAudioProcessorFrameSamples;
      AudioProcessorFrame near{};
      std::copy_n(frame.samples.begin() + static_cast<std::ptrdiff_t>(offset),
                  kAudioProcessorFrameSamples, near.begin());
      if (!Process10msWindow(near, &processed, offset)) {
        return false;
      }
    }
    const auto validation = domain::validate_audio_frame(AudioFrame{
        domain::kAudioSampleRateHz, domain::kAudioChannels,
        domain::AudioSampleFormat::kS16LE,
        std::vector<std::int16_t>(processed.begin(), processed.end())});
    if (!validation.ok()) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      processor_error_ = Error{ErrorCode::kBackendFailure,
                               "前处理输出不符合统一音频契约"};
      return false;
    }
    EnqueueProcessed(validation.value);
    return true;
  }

  bool Process10msWindow(const AudioProcessorFrame& near,
                         std::array<std::int16_t, domain::kAudioFrameSamples>* output,
                         std::size_t output_offset) {
    std::string reset_reason;
    {
      std::lock_guard<std::mutex> lock(render_mutex_);
      reset_reason = timeline_.TakePendingResetReasonLocked();
    }
    if (!reset_reason.empty()) {
      if (!ResetProcessor(reset_reason)) {
        return false;
      }
    }

    AudioProcessorFrame render{};
    std::size_t zero_filled = 0;
    std::size_t late_samples = 0;
    bool late_exceeded = false;
    std::size_t consumed = 0;
    {
      std::lock_guard<std::mutex> lock(render_mutex_);
      consumed = timeline_.PopReadyLocked(
          NowSteadyNs(), render.data(), render.size(),
          config_.reference_schedule_tolerance.count() * 1000000LL,
          config_.max_reference_lateness.count() * 1000000LL,
          &zero_filled, &late_samples, &late_exceeded);
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      stats_.render_reference_samples_consumed += consumed;
      stats_.render_reference_zero_filled += zero_filled;
      stats_.render_reference_late_samples += late_samples;
    }
    if (late_exceeded) {
      MarkReferenceDiscontinuity("播放参考迟到超过预算");
    }

    domain::Result<AudioProcessorFrame> captured;
    {
      std::lock_guard<std::mutex> lock(processor_mutex_);
      const auto rendered = processor_->process_render(render);
      if (!rendered.ok()) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        ++stats_.processor_failures;
        processor_error_ = rendered.error;
        return false;
      }
      captured = processor_->process_capture(near);
      if (!captured.ok()) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        ++stats_.processor_failures;
        processor_error_ = captured.error;
        return false;
      }
    }
    std::copy(captured.value->begin(), captured.value->end(),
              output->begin() + static_cast<std::ptrdiff_t>(output_offset));
    return true;
  }

  bool ResetProcessor(const std::string& reason) {
    domain::OperationResult result;
    {
      std::lock_guard<std::mutex> lock(processor_mutex_);
      result = processor_->reset(reason);
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++stats_.processor_resets;
      if (!result.ok()) {
        ++stats_.processor_failures;
        processor_error_ = result.error;
      }
    }
    return result.ok();
  }

  bool RecoverCapture(const Error& failure) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (capture_closing_ || capture_cancelled_) {
        return false;
      }
      capture_state_ = AudioPipelineState::kRecovering;
      ++stats_.capture_reconnect_attempts;
      capture_error_ = failure;
    }
    if (capture_) {
      (void)capture_->close();
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          config_.reconnect_total_budget;
    Error last_error = failure;
    while (true) {
      if (!SleepRetryInterval()) {
        return false;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        break;
      }
      const auto opened = capture_->open();
      if (opened.ok()) {
        domain::OperationResult reset;
        {
          std::lock_guard<std::mutex> lock(processor_mutex_);
          reset = processor_->reset("采集设备重连后重新对齐");
        }
        if (!reset.ok()) {
          {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ++stats_.processor_failures;
            processor_error_ = reset.error;
            capture_state_ = AudioPipelineState::kFailed;
            capture_error_ = reset.error;
            capture_terminal_ = true;
          }
          processed_cv_.notify_all();
          return false;
        }
        MarkReferenceDiscontinuity("采集设备重连后重新对齐");
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          capture_state_ = AudioPipelineState::kReady;
          capture_error_ = Error{};
          ++stats_.capture_reconnect_successes;
        }
        return true;
      }
      last_error = opened.error;
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++stats_.capture_reconnect_failures;
      capture_state_ = AudioPipelineState::kFailed;
      capture_error_ = last_error;
      capture_terminal_ = true;
    }
    processed_cv_.notify_all();
    return false;
  }

  bool SleepRetryInterval() {
    const auto deadline = std::chrono::steady_clock::now() +
                          config_.reconnect_retry_interval;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (capture_closing_ || capture_cancelled_) {
          return false;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  void FinishCaptureTerminal(const Error& error, bool failed) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      capture_terminal_ = true;
      capture_error_ = error;
      if (failed) {
        capture_state_ = AudioPipelineState::kFailed;
      } else {
        capture_state_ = AudioPipelineState::kStopped;
      }
    }
    processed_cv_.notify_all();
  }

  void EnqueueProcessed(AudioFrame frame) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (capture_closing_ || capture_cancelled_) {
        return;
      }
      while (processed_.size() >= config_.processed_frame_queue_capacity) {
        processed_.pop_front();
        ++stats_.capture_dropped_frames;
      }
      processed_.push_back(std::move(frame));
    }
    processed_cv_.notify_all();
  }

  void RecordCaptureOpenFailure(const Error& error) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    capture_open_ = false;
    capture_state_ = AudioPipelineState::kFailed;
    capture_error_ = error;
  }

  // 播放参考 ---------------------------------------------------------------

  void OnPlaybackReference(const backend::AlsaPlaybackReferenceBlock& block) {
    if (block.kind == backend::AlsaPlaybackReferenceKind::kDiscontinuity) {
      MarkReferenceDiscontinuity(block.reason.empty()
                                     ? std::string("播放参考不连续")
                                     : block.reason);
      return;
    }
    if (!block.continuous) {
      MarkReferenceDiscontinuity("播放参考连续标志为假");
    }
    bool appended = false;
    bool overflow = false;
    std::size_t produced = 0;
    {
      std::lock_guard<std::mutex> lock(render_mutex_);
      appended = timeline_.AppendBlockLocked(block, NowSteadyNs(), &produced);
      overflow = timeline_.queued_samples() >
                 config_.render_reference_sample_capacity;
      if (overflow) {
        timeline_.MarkDiscontinuityLocked("播放参考队列溢出");
      }
    }
    if (!appended) {
      MarkReferenceDiscontinuity("播放参考格式或时间信息不可用");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++stats_.render_reference_blocks;
      stats_.render_reference_samples_queued += produced;
      if (overflow) {
        ++stats_.render_reference_overflows;
      }
    }
    if (overflow) {
      MarkReferenceDiscontinuity("播放参考队列溢出");
    }
  }

  void MarkReferenceDiscontinuity(const std::string& reason) {
    {
      std::lock_guard<std::mutex> lock(render_mutex_);
      timeline_.MarkDiscontinuityLocked(reason);
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++stats_.render_reference_discontinuities;
    }
  }

  AudioFrontendConfig config_;
  std::unique_ptr<backend::AlsaAudioSource> capture_;
  std::unique_ptr<backend::AlsaAudioSink> playback_;
  std::unique_ptr<IAudioProcessor> processor_;

  mutable std::mutex state_mutex_;
  mutable std::mutex processor_mutex_;
  mutable std::mutex render_mutex_;
  std::condition_variable processed_cv_;
  RenderReferenceTimeline timeline_;
  std::deque<AudioFrame> processed_;
  std::thread capture_thread_;
  bool capture_thread_joinable_ = false;

  bool capture_open_ = false;
  bool capture_closing_ = false;
  bool capture_cancelled_ = false;
  bool capture_terminal_ = false;
  AudioPipelineState capture_state_ = AudioPipelineState::kStopped;
  Error capture_error_{};

  bool playback_open_ = false;
  bool playback_closing_ = false;
  bool playback_cancelled_ = false;
  AudioPipelineState playback_state_ = AudioPipelineState::kStopped;
  Error playback_error_{};

  Error processor_error_{};
  AudioFrontendStats stats_;
};

DuplexAudioFrontend::DuplexAudioFrontend(
    AudioFrontendConfig config,
    std::unique_ptr<backend::PcmCaptureBackend> capture_backend,
    std::unique_ptr<backend::PcmPlaybackBackend> playback_backend,
    std::unique_ptr<IAudioProcessor> processor)
    : impl_(new Impl(std::move(config), std::move(capture_backend),
                     std::move(playback_backend), std::move(processor))) {
  source_.reset(new FrontendAudioSource(*this));
  sink_.reset(new FrontendAudioSink(*this));
}

DuplexAudioFrontend::~DuplexAudioFrontend() {
  if (impl_ != nullptr) {
    (void)impl_->CloseCapture();
    (void)impl_->ClosePlayback();
  }
}

FrontendAudioSource& DuplexAudioFrontend::source() noexcept {
  return *source_;
}

FrontendAudioSink& DuplexAudioFrontend::sink() noexcept {
  return *sink_;
}

AudioPipelineState DuplexAudioFrontend::capture_state() const {
  return impl_ == nullptr ? AudioPipelineState::kStopped : impl_->capture_state();
}

AudioPipelineState DuplexAudioFrontend::playback_state() const {
  return impl_ == nullptr ? AudioPipelineState::kStopped : impl_->playback_state();
}

AudioProcessorState DuplexAudioFrontend::aec_state() const {
  return impl_ == nullptr ? AudioProcessorState::kClosed : impl_->aec_state();
}

Error DuplexAudioFrontend::capture_error() const {
  return impl_ == nullptr ? Error{} : impl_->capture_error();
}

Error DuplexAudioFrontend::playback_error() const {
  return impl_ == nullptr ? Error{} : impl_->playback_error();
}

Error DuplexAudioFrontend::processor_error() const {
  return impl_ == nullptr ? Error{} : impl_->processor_error();
}

backend::PcmCaptureFormat DuplexAudioFrontend::actual_capture_format() const noexcept {
  return impl_ == nullptr ? backend::PcmCaptureFormat{}
                          : impl_->actual_capture_format();
}

backend::PcmPlaybackFormat DuplexAudioFrontend::actual_playback_format() const noexcept {
  return impl_ == nullptr ? backend::PcmPlaybackFormat{}
                          : impl_->actual_playback_format();
}

AudioFrontendStats DuplexAudioFrontend::stats() const {
  return impl_ == nullptr ? AudioFrontendStats{} : impl_->stats();
}

domain::OperationResult DuplexAudioFrontend::OpenCapture() {
  return impl_ == nullptr
             ? OperationResult::failure(ErrorCode::kInvalidInput,
                                        "全双工音频前端未初始化")
             : impl_->OpenCapture();
}

domain::Result<AudioFrame> DuplexAudioFrontend::ReadProcessed() {
  return impl_ == nullptr
             ? domain::Result<AudioFrame>::failure(ErrorCode::kInvalidInput,
                                                   "全双工音频前端未初始化")
             : impl_->ReadProcessed();
}

domain::OperationResult DuplexAudioFrontend::CancelCapture() noexcept {
  return impl_ == nullptr ? OperationResult::success() : impl_->CancelCapture();
}

domain::OperationResult DuplexAudioFrontend::CloseCapture() noexcept {
  return impl_ == nullptr ? OperationResult::success() : impl_->CloseCapture();
}

domain::OperationResult DuplexAudioFrontend::OpenPlayback() {
  return impl_ == nullptr
             ? OperationResult::failure(ErrorCode::kInvalidInput,
                                        "全双工音频前端未初始化")
             : impl_->OpenPlayback();
}

domain::OperationResult DuplexAudioFrontend::WritePlayback(const AudioFrame& frame) {
  return impl_ == nullptr
             ? OperationResult::failure(ErrorCode::kInvalidInput,
                                        "全双工音频前端未初始化")
             : impl_->WritePlayback(frame);
}

domain::OperationResult DuplexAudioFrontend::CancelPlayback() noexcept {
  return impl_ == nullptr ? OperationResult::success() : impl_->CancelPlayback();
}

domain::OperationResult DuplexAudioFrontend::ClosePlayback() noexcept {
  return impl_ == nullptr ? OperationResult::success() : impl_->ClosePlayback();
}

FrontendAudioSource::FrontendAudioSource(DuplexAudioFrontend& owner) noexcept
    : owner_(owner) {}

FrontendAudioSource::~FrontendAudioSource() {
  (void)owner_.CloseCapture();
}

domain::OperationResult FrontendAudioSource::open() {
  return owner_.OpenCapture();
}

domain::Result<AudioFrame> FrontendAudioSource::read() {
  return owner_.ReadProcessed();
}

domain::OperationResult FrontendAudioSource::cancel() noexcept {
  return owner_.CancelCapture();
}

domain::OperationResult FrontendAudioSource::close() noexcept {
  return owner_.CloseCapture();
}

FrontendAudioSink::FrontendAudioSink(DuplexAudioFrontend& owner) noexcept
    : owner_(owner) {}

FrontendAudioSink::~FrontendAudioSink() {
  (void)owner_.ClosePlayback();
}

domain::OperationResult FrontendAudioSink::open() {
  return owner_.OpenPlayback();
}

domain::OperationResult FrontendAudioSink::write(const AudioFrame& frame) {
  return owner_.WritePlayback(frame);
}

domain::OperationResult FrontendAudioSink::cancel() noexcept {
  return owner_.CancelPlayback();
}

domain::OperationResult FrontendAudioSink::close() noexcept {
  return owner_.ClosePlayback();
}

}  // namespace nexweave::runtime

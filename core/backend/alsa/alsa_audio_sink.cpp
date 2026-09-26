#include "alsa_audio_sink.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace nexweave::backend {
namespace {

using domain::AudioFrame;
using domain::ErrorCode;
using domain::OperationResult;

std::int64_t NowSteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 把统一 16 kHz 单声道 S16_LE 帧转换成设备原生交错样本。
//
// 职责：显式重采样、声道复制和样本编码转换；保存跨帧相位，保证连续帧不会因
// 每帧独立转换而在边界产生额外跳变。输入前提：Configure 已设置有效原生格式，
// 调用方已按 AudioFrame 契约校验帧。输出后置：native 中的每个 int32 样本都属于
// 同一格式；调用方必须把全部输出提交给后端后才算消费了本帧。
// 边界：重采样在最后一个可用源样本上使用零阶保持，不等待下一帧；这是实时播放
// 避免额外一帧延迟的取舍，边界误差限制在一个采样内，不能声称与离线带限重采样等价。
class PlaybackPcmConverter {
 public:
  // 设置原生格式并清除上一轮状态。格式无效时处于未配置状态，后续 ConvertFrame 失败。
  void Configure(const PcmPlaybackFormat& format) {
    Reset();
    if (format.valid()) {
      format_ = format;
      format_known_ = true;
    }
  }

  void Reset() {
    input_.clear();
    input_base_ = 0;
    total_input_samples_ = 0;
    total_output_samples_ = 0;
    last_input_sample_ = 0;
    format_ = PcmPlaybackFormat{};
    format_known_ = false;
  }

  // 转换一帧并追加到 native 输出。调用方保证 native 非空且拥有独立存储。
  OperationResult ConvertFrame(const AudioFrame& frame, std::vector<std::int32_t>* native) {
    if (native == nullptr) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "原生输出缓冲为空");
    }
    if (!format_known_ || !format_.valid()) {
      return OperationResult::failure(ErrorCode::kBackendFailure,
                                      "播放后端格式尚未配置");
    }
    if (!domain::validate_audio_frame(frame).ok()) {
      return OperationResult::failure(ErrorCode::kInvalidInput,
                                      "播放帧不符合统一音频契约");
    }

    native->clear();
    for (std::int16_t sample : frame.samples) {
      input_.push_back(sample);
      last_input_sample_ = sample;
    }
    total_input_samples_ += frame.samples.size();

    const std::uint64_t target_output =
        total_input_samples_ * format_.sample_rate_hz / domain::kAudioSampleRateHz;
    while (total_output_samples_ < target_output) {
      const double source_position =
          static_cast<double>(total_output_samples_) * domain::kAudioSampleRateHz /
          static_cast<double>(format_.sample_rate_hz);
      const auto source_index_floor =
          static_cast<std::uint64_t>(std::floor(source_position));
      const std::uint64_t local_index =
          source_index_floor >= input_base_ ? source_index_floor - input_base_ : 0;
      std::int16_t mono = last_input_sample_;
      if (local_index < input_.size()) {
        mono = input_[local_index];
        if (local_index + 1U < input_.size()) {
          const double fraction =
              source_position - static_cast<double>(source_index_floor);
          const double interpolated =
              (1.0 - fraction) * static_cast<double>(mono) +
              fraction * static_cast<double>(input_[local_index + 1U]);
          const long rounded = std::lround(interpolated);
          mono = static_cast<std::int16_t>(std::clamp(rounded, -32768L, 32767L));
        }
      }
      AppendNativeSample(mono, native);
      ++total_output_samples_;
    }

    const std::uint64_t next_source_index = static_cast<std::uint64_t>(
        std::floor(static_cast<double>(total_output_samples_) *
                   domain::kAudioSampleRateHz / format_.sample_rate_hz));
    while (!input_.empty() && input_base_ < next_source_index) {
      input_.pop_front();
      ++input_base_;
    }
    if (input_.empty()) {
      input_base_ = total_input_samples_;
    }
    return OperationResult::success();
  }

 private:
  void AppendNativeSample(std::int16_t mono, std::vector<std::int32_t>* native) {
    const std::int32_t value = format_.format == PlaybackSampleFormat::kS16LE
                                   ? static_cast<std::int32_t>(mono)
                                   : static_cast<std::int32_t>(mono) * 256;
    native->push_back(value);
    if (format_.channels == 2) {
      native->push_back(value);
    }
  }

  PcmPlaybackFormat format_{};
  bool format_known_ = false;
  std::deque<std::int16_t> input_;
  std::uint64_t input_base_ = 0;
  std::uint64_t total_input_samples_ = 0;
  std::uint64_t total_output_samples_ = 0;
  std::int16_t last_input_sample_ = 0;
};

std::string ErrorOrFallback(const std::string& detail, const std::string& fallback) {
  return detail.empty() ? fallback : detail;
}

}  // namespace

domain::OperationResult validate_pcm_playback_backend_config(
    const PcmPlaybackBackendConfig& config) {
  if (config.device.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "播放设备名不能为空");
  }
  if (config.sample_rate_hz == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "播放采样率必须大于 0");
  }
  if (config.channels == 0 || config.channels > 2) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "播放声道数只支持 1 或 2");
  }
  if (config.period_frames == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "period_frames 必须大于 0");
  }
  if (config.buffer_frames < config.period_frames) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "buffer_frames 不能小于 period_frames");
  }
  if (config.write_poll_slice_ms == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "write_poll_slice_ms 必须大于 0");
  }
  if (config.drain_poll_slice_ms == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "drain_poll_slice_ms 必须大于 0");
  }
  return OperationResult::success();
}

domain::OperationResult validate_alsa_audio_sink_config(
    const AlsaAudioSinkConfig& config) {
  if (config.write_timeout.count() <= 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "write_timeout 必须大于 0");
  }
  if (config.max_backend_writes_per_frame == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "max_backend_writes_per_frame 必须大于 0");
  }
  if (config.drain_timeout.count() <= 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "drain_timeout 必须大于 0");
  }
  return OperationResult::success();
}

struct AlsaAudioSink::Impl {
  AlsaAudioSinkConfig config_;
  std::unique_ptr<PcmPlaybackBackend> backend_;

  // state_mutex_ 保护轮次状态、转换器、参考序和统计；device_mutex_ 串行化
  // open/write/recover/drain/close。cancel 只写原子/状态标志并调用后端取消，
  // 不等待 device_mutex_，从而有限唤醒阻塞 write。
  mutable std::mutex state_mutex_;
  std::mutex device_mutex_;
  std::condition_variable write_idle_cv_;
  PlaybackPcmConverter converter_;
  bool opened_ = false;
  bool closed_ = false;
  bool cancelled_ = false;
  bool closing_ = false;
  bool write_in_progress_ = false;
  bool failed_ = false;
  bool continuous_ = true;
  std::uint64_t reference_sequence_ = 0;
  domain::Error last_error_{};
  AlsaPlaybackReferenceCallback reference_callback_;
  AlsaAudioSinkStats stats_;

  Impl(AlsaAudioSinkConfig config, std::unique_ptr<PcmPlaybackBackend> backend)
      : config_(std::move(config)), backend_(std::move(backend)) {}

  OperationResult open() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      stats_ = AlsaAudioSinkStats{};
      ++stats_.open_attempts;
      if (opened_ && !closed_) {
        return OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                        "音频输出已经打开");
      }
      if (!opened_ && cancelled_) {
        return OperationResult::failure(ErrorCode::kCancelled,
                                        "音频输出已经取消，需要先 close");
      }
      if (!backend_) {
        return OperationResult::failure(ErrorCode::kInvalidInput,
                                        "未提供 PCM 播放后端");
      }
      closed_ = false;
    }

    OperationResult opened;
    {
      std::lock_guard<std::mutex> device_lock(device_mutex_);
      opened = backend_->open();
    }
    if (!opened.ok()) {
      return opened;
    }

    bool cancel_during_open = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (cancelled_ || closing_) {
        cancel_during_open = true;
      } else {
        const PcmPlaybackFormat format = backend_->current_format();
        converter_.Configure(format);
        opened_ = true;
        closed_ = false;
        cancelled_ = false;
        failed_ = false;
        continuous_ = true;
        reference_sequence_ = 0;
        last_error_ = domain::Error{};
        ++stats_.open_successes;
        stats_.actual_sample_rate_hz = format.sample_rate_hz;
        stats_.actual_channels = format.channels;
        stats_.actual_format = format.format;
        const PcmPlaybackBackendStats backend_stats = backend_->stats();
        stats_.actual_period_frames = backend_stats.actual_period_frames;
        stats_.actual_buffer_frames = backend_stats.actual_buffer_frames;
      }
    }
    if (cancel_during_open) {
      std::lock_guard<std::mutex> device_lock(device_mutex_);
      (void)backend_->close();
      return OperationResult::failure(ErrorCode::kCancelled,
                                      "音频输出打开期间被取消");
    }
    return OperationResult::success();
  }

  OperationResult write(const AudioFrame& frame) {
    const auto valid = domain::validate_audio_frame(frame);
    if (!valid.ok()) {
      return OperationResult::failure(ErrorCode::kInvalidInput,
                                      "播放帧不符合统一音频契约");
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++stats_.write_calls;
      if (!opened_ || closed_ || closing_) {
        return OperationResult::failure(ErrorCode::kDeviceFailure,
                                        "音频输出尚未打开或已经关闭");
      }
      if (cancelled_) {
        return OperationResult::failure(ErrorCode::kCancelled,
                                        "音频输出已经取消");
      }
      if (failed_) {
        return OperationResult::failure(last_error_.code,
                                        ErrorOrFallback(last_error_.message,
                                                        "音频输出已经失败"));
      }
      write_in_progress_ = true;
    }
    WriteScope write_scope(this);

    std::vector<std::int32_t> native;
    OperationResult converted;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      converted = converter_.ConvertFrame(frame, &native);
    }
    if (!converted.ok()) {
      return converted;
    }

    const std::size_t channels = backend_->current_format().channels;
    if (channels == 0) {
      return OperationResult::failure(ErrorCode::kDeviceFailure,
                                      "播放后端没有有效声道数");
    }
    const std::size_t native_frames = native.size() / channels;
    const PcmPlaybackFormat native_format = backend_->current_format();
    std::size_t offset_frames = 0;
    std::size_t attempts = 0;
    std::size_t recoveries = 0;
    std::size_t reconnects = 0;
    const auto deadline = std::chrono::steady_clock::now() + config_.write_timeout;

    while (offset_frames < native_frames) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (cancelled_) {
          return OperationResult::failure(ErrorCode::kCancelled,
                                          "音频输出已经取消");
        }
        if (closed_ || closing_ || !opened_) {
          return OperationResult::failure(ErrorCode::kDeviceFailure,
                                          "音频输出已经关闭");
        }
      }
      if (attempts >= config_.max_backend_writes_per_frame) {
        return Fail(ErrorCode::kTimeout, "播放后端写入次数超过预算");
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return Fail(ErrorCode::kTimeout, "播放写入等待超过预算");
      }
      auto remaining_time =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      if (remaining_time.count() <= 0) {
        remaining_time = std::chrono::milliseconds(1);
      }

      const std::size_t remaining_frames = native_frames - offset_frames;
      const std::int32_t* samples =
          native.data() + offset_frames * channels;
      const std::int64_t write_begin_ns = NowSteadyNs();
      PcmPlaybackWriteResult result;
      {
        std::lock_guard<std::mutex> device_lock(device_mutex_);
        result = backend_->write(samples, remaining_frames, remaining_time);
        ++attempts;
      }
      const std::int64_t write_done_ns = NowSteadyNs();

      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++stats_.backend_writes;
        if (cancelled_) {
          return OperationResult::failure(ErrorCode::kCancelled,
                                          "音频输出已经取消");
        }
        if (closed_ || closing_ || !opened_) {
          return OperationResult::failure(ErrorCode::kDeviceFailure,
                                          "音频输出已经关闭");
        }
      }

      switch (result.kind) {
        case PcmPlaybackWriteKind::kWritten: {
          const std::size_t accepted =
              std::min(result.frames_written, remaining_frames);
          if (accepted == 0) {
            return Fail(ErrorCode::kBackendFailure,
                        "播放后端返回了零帧写入成功");
          }
          if (accepted < remaining_frames) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ++stats_.short_writes;
          }
          PublishAudioReference(samples, accepted, write_begin_ns, write_done_ns,
                                result.delay_frames);
          offset_frames += accepted;
          continue;
        }
        case PcmPlaybackWriteKind::kRecovered:
          if (recoveries >= config_.max_recoveries_per_frame) {
            return Fail(ErrorCode::kDeviceFailure,
                        "底层播放恢复次数超过预算");
          }
          ++recoveries;
          {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ++stats_.underruns;
            continuous_ = false;
          }
          PublishDiscontinuity("underrun 或播放流状态恢复");
          continue;
        case PcmPlaybackWriteKind::kTimeout:
          {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ++stats_.timeouts;
          }
          return Fail(ErrorCode::kTimeout,
                      ErrorOrFallback(result.error.message, "播放写入超时"));
        case PcmPlaybackWriteKind::kCancelled:
          {
            std::lock_guard<std::mutex> lock(state_mutex_);
            cancelled_ = true;
            continuous_ = false;
          }
          PublishDiscontinuity("播放写入被取消");
          return OperationResult::failure(ErrorCode::kCancelled,
                                          "音频输出已经取消");
        case PcmPlaybackWriteKind::kAlreadyCompleted:
          return Fail(ErrorCode::kAlreadyCompleted, "播放后端已经关闭");
        case PcmPlaybackWriteKind::kDeviceFailure:
          {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ++stats_.device_failures;
          }
          if (reconnects >= config_.reconnect_budget) {
            return Fail(ErrorCode::kDeviceFailure,
                        ErrorOrFallback(result.error.message, "播放设备写入失败"));
          }
          ++reconnects;
          {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ++stats_.reconnect_attempts;
            continuous_ = false;
          }
          OperationResult recovered;
          {
            std::lock_guard<std::mutex> device_lock(device_mutex_);
            recovered = backend_->recover();
          }
          if (!recovered.ok()) {
            return Fail(recovered.error.code == ErrorCode::kNone
                            ? ErrorCode::kDeviceFailure
                            : recovered.error.code,
                        ErrorOrFallback(recovered.error.message, "播放设备重连失败"));
          }
          {
            const PcmPlaybackFormat recovered_format = backend_->current_format();
            if (recovered_format.sample_rate_hz != native_format.sample_rate_hz ||
                recovered_format.channels != native_format.channels ||
                recovered_format.format != native_format.format) {
              return Fail(ErrorCode::kDeviceFailure,
                          "播放设备重连后原生格式变化，拒绝继续写旧转换数据");
            }
            std::lock_guard<std::mutex> lock(state_mutex_);
            ++stats_.reconnect_successes;
          }
          PublishDiscontinuity("播放设备重连成功");
          continue;
      }
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      stats_.frames_consumed += domain::kAudioFrameSamples;
    }
    return OperationResult::success();
  }

  OperationResult cancel() noexcept {
    bool should_cancel_backend = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (closed_ || !opened_) {
        return OperationResult::success();
      }
      if (!cancelled_) {
        cancelled_ = true;
        continuous_ = false;
        converter_.Reset();
        should_cancel_backend = true;
      } else {
        return OperationResult::success();
      }
    }
    PublishDiscontinuity("播放轮次被取消");
    if (should_cancel_backend && backend_) {
      const auto cancelled = backend_->cancel();
      if (!cancelled.ok()) {
        return cancelled;
      }
    }
    return OperationResult::success();
  }

  OperationResult close() noexcept {
    bool force_drop = false;
    bool must_close_backend = false;
    bool had_writer = false;
    domain::Error drain_error{};
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++stats_.close_calls;
      if (closed_ && !opened_) {
        return OperationResult::success();
      }
      closing_ = true;
      force_drop = cancelled_;
      had_writer = write_in_progress_;
      if (had_writer) {
        force_drop = true;
        cancelled_ = true;
      }
      closed_ = true;
      opened_ = false;
      converter_.Reset();
      must_close_backend = backend_ != nullptr;
    }

    if (had_writer && backend_) {
      (void)backend_->cancel();
    }
    if (must_close_backend && !force_drop) {
      OperationResult drained;
      {
        std::lock_guard<std::mutex> device_lock(device_mutex_);
        drained = backend_->drain(config_.drain_timeout);
      }
      if (drained.ok()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++stats_.drained;
      } else {
        drain_error = drained.error;
      }
    }

    OperationResult close_result;
    if (must_close_backend) {
      std::lock_guard<std::mutex> device_lock(device_mutex_);
      close_result = backend_->close();
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      closing_ = false;
      closed_ = true;
      opened_ = false;
      cancelled_ = false;
      failed_ = false;
      continuous_ = true;
      write_idle_cv_.notify_all();
    }
    if (!drain_error.ok()) {
      return OperationResult::failure(drain_error.code, drain_error.message);
    }
    return close_result;
  }

  PcmPlaybackFormat actual_format() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!opened_ || backend_ == nullptr) {
      return PcmPlaybackFormat{};
    }
    return backend_->current_format();
  }

  void set_reference_callback(AlsaPlaybackReferenceCallback callback) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    reference_callback_ = std::move(callback);
  }

  AlsaAudioSinkStats stats() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return stats_;
  }

  OperationResult Fail(ErrorCode code, std::string message) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      failed_ = true;
      last_error_.code = code;
      last_error_.message = message;
    }
    return OperationResult::failure(code, std::move(message));
  }

  void PublishAudioReference(const std::int32_t* samples, std::size_t frames,
                             std::int64_t write_begin_ns, std::int64_t write_done_ns,
                             std::int64_t delay_frames) {
    if (samples == nullptr || frames == 0 || !backend_) {
      return;
    }
    AlsaPlaybackReferenceBlock block;
    block.kind = AlsaPlaybackReferenceKind::kAudio;
    block.format = backend_->current_format();
    if (!block.format.valid()) {
      return;
    }
    const std::size_t sample_count = frames * block.format.channels;
    block.interleaved_samples.assign(samples, samples + sample_count);
    block.write_begin_steady_ns = write_begin_ns;
    block.write_done_steady_ns = write_done_ns;
    block.delay_frames = delay_frames;
    AlsaPlaybackReferenceCallback callback;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      block.sequence = reference_sequence_++;
      block.continuous = continuous_;
      continuous_ = true;
      ++stats_.reference_blocks;
      callback = reference_callback_;
    }
    if (callback) {
      try {
        callback(block);
      } catch (...) {
        // 回放参考不能把播放线程从设备写入路径上抛出去；任务 44 只保证参考
        // 事件已经提供，回放错误仍由 write 的返回码表达。回调实现必须自行
        // 保证不抛异常，这里兜底只是防止音频线程被第三方异常打断。
      }
    }
  }

  void PublishDiscontinuity(const std::string& reason) {
    AlsaPlaybackReferenceBlock block;
    block.kind = AlsaPlaybackReferenceKind::kDiscontinuity;
    block.continuous = false;
    block.reason = reason;
    block.write_begin_steady_ns = NowSteadyNs();
    block.write_done_steady_ns = block.write_begin_steady_ns;
    AlsaPlaybackReferenceCallback callback;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      block.sequence = reference_sequence_++;
      ++stats_.reference_discontinuities;
      callback = reference_callback_;
    }
    if (callback) {
      try {
        callback(block);
      } catch (...) {
        // 同上：不连续通知不得反向破坏播放状态机。
      }
    }
  }

  struct WriteScope {
    explicit WriteScope(Impl* impl) : impl_(impl) {}
    ~WriteScope() {
      std::lock_guard<std::mutex> lock(impl_->state_mutex_);
      impl_->write_in_progress_ = false;
      impl_->write_idle_cv_.notify_all();
    }
    Impl* impl_;
  };
};

AlsaAudioSink::AlsaAudioSink(AlsaAudioSinkConfig config,
                             std::unique_ptr<PcmPlaybackBackend> backend)
    : impl_(new Impl(std::move(config), std::move(backend))) {}

AlsaAudioSink::~AlsaAudioSink() {
  if (impl_ != nullptr) {
    (void)impl_->close();
  }
}

domain::OperationResult AlsaAudioSink::open() {
  if (impl_ == nullptr) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "适配器未初始化");
  }
  const auto valid = validate_alsa_audio_sink_config(impl_->config_);
  if (!valid.ok()) {
    return valid;
  }
  return impl_->open();
}

domain::OperationResult AlsaAudioSink::write(const domain::AudioFrame& frame) {
  if (impl_ == nullptr) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "适配器未初始化");
  }
  return impl_->write(frame);
}

domain::OperationResult AlsaAudioSink::cancel() noexcept {
  if (impl_ == nullptr) {
    return OperationResult::success();
  }
  return impl_->cancel();
}

domain::OperationResult AlsaAudioSink::close() noexcept {
  if (impl_ == nullptr) {
    return OperationResult::success();
  }
  return impl_->close();
}

PcmPlaybackFormat AlsaAudioSink::actual_playback_format() const noexcept {
  if (impl_ == nullptr) {
    return PcmPlaybackFormat{};
  }
  return impl_->actual_format();
}

void AlsaAudioSink::set_playback_reference_callback(
    AlsaPlaybackReferenceCallback callback) noexcept {
  if (impl_ != nullptr) {
    impl_->set_reference_callback(std::move(callback));
  }
}

AlsaAudioSinkStats AlsaAudioSink::stats() const noexcept {
  if (impl_ == nullptr) {
    return AlsaAudioSinkStats{};
  }
  return impl_->stats();
}

}  // namespace nexweave::backend

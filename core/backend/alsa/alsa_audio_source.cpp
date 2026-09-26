#include "alsa_audio_source.hpp"

#include <algorithm>
#include <cmath>
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
using domain::Result;

// 设备原生块到统一 16 kHz 单声道 S16_LE 帧的转换器。
//
// 职责：把后端返回的原生采样率、1～2 声道、S16_LE/S24_LE 交错样本显式转换为
// 16 kHz 单声道 16 位样本，并保存跨块状态。它不做设备访问、线程调度或协议编码。
// 输入前提：每个块格式在转换器 Reset 后首次出现时确定；后续块格式变化视为契约
// 违反，因为采样率/声道/编码变化会让跨块时间轴失去意义，必须由调用方显式 Reset。
// 输出后置：Append 成功时，产出的每个样本都属于同一转换状态；调用方可以从输出
// 队列按 320 样本切帧。取消、关闭或设备恢复时调用方必须 Reset，丢弃旧缓存。
class PcmFrameConverter {
 public:
  // 清除全部原生样本、重采样相位和格式记忆。释放缓存内存，不访问设备。
  void Reset() {
    native_samples_.clear();
    output_.clear();
    native_base_sample_ = 0;
    next_output_position_ = 0.0;
    format_known_ = false;
    format_ = PcmCaptureFormat{};
  }

  // 追加一个原生块并产出尽可能多的 16 kHz 单声道样本。返回本次新增输出样本数；
  // 块为空、格式非法、帧数不完整或格式在中途变化时返回 kBackendFailure。
  Result<std::size_t> Append(const PcmCaptureBlock& block) {
    if (!block.format.valid()) {
      return Result<std::size_t>::failure(ErrorCode::kBackendFailure,
                                          "设备返回了无效的原生音频格式");
    }
    if (block.frame_count() == 0) {
      return Result<std::size_t>::failure(ErrorCode::kBackendFailure,
                                          "设备返回了空数据块或非整数帧样本");
    }
    for (std::int32_t sample : block.interleaved_samples) {
      if (!NativeSampleInRange(sample, block.format.format)) {
        return Result<std::size_t>::failure(ErrorCode::kBackendFailure,
                                            "设备样本值超出声明编码范围");
      }
    }
    if (!format_known_) {
      format_ = block.format;
      format_known_ = true;
    } else if (format_.sample_rate_hz != block.format.sample_rate_hz ||
               format_.channels != block.format.channels ||
               format_.format != block.format.format) {
      return Result<std::size_t>::failure(
          ErrorCode::kBackendFailure,
          "设备原生格式在未 Reset 的转换状态中发生变化");
    }

    const std::size_t produced_before = output_.size();
    const std::size_t channels = block.format.channels;
    const std::size_t frames = block.frame_count();
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const std::size_t offset = frame * channels;
      std::int32_t mono = 0;
      if (channels == 1) {
        mono = NormalizeTo24Bit(block.interleaved_samples[offset],
                                block.format.format);
      } else {
        const std::int32_t left =
            NormalizeTo24Bit(block.interleaved_samples[offset],
                             block.format.format);
        const std::int32_t right =
            NormalizeTo24Bit(block.interleaved_samples[offset + 1],
                             block.format.format);
        mono = static_cast<std::int32_t>(
            (static_cast<std::int64_t>(left) + static_cast<std::int64_t>(right)) / 2);
      }
      native_samples_.push_back(static_cast<std::int16_t>(mono >> 8));
    }
    DrainResampler();
    return Result<std::size_t>::success(output_.size() - produced_before);
  }

  // 尚未被重采样消费的原生单声道样本数，用于恢复时统计丢弃量。
  std::size_t pending_native_samples() const noexcept {
    return native_samples_.size();
  }

  // 取出一个完整输出帧；调用方必须先用 output_ready() 确认样本充足。
  void PopFrame(std::vector<std::int16_t>* samples) {
    samples->resize(domain::kAudioFrameSamples);
    for (std::size_t index = 0; index < domain::kAudioFrameSamples; ++index) {
      (*samples)[index] = output_.front();
      output_.pop_front();
    }
  }

  bool output_ready() const noexcept {
    return output_.size() >= domain::kAudioFrameSamples;
  }

  std::size_t output_pending() const noexcept {
    return output_.size();
  }

  // 当前是否已经确立原生格式。测试和诊断可用来判断恢复后格式记忆是否已清除。
  bool format_known() const noexcept {
    return format_known_;
  }

 private:
  // 把 S16_LE 或 S24_LE 原生值扩展为统一的 24 位有符号范围。
  static std::int32_t NormalizeTo24Bit(std::int32_t sample,
                                       PcmSampleFormat format) noexcept {
    if (format == PcmSampleFormat::kS16LE) {
      const auto signed16 = static_cast<std::int16_t>(sample);
      return static_cast<std::int32_t>(signed16) * 256;
    }
    return sample;
  }

  // 检查样本是否落在其编码的合法范围内，防止后端误把 S24 值塞进 S16 路径。
  static bool NativeSampleInRange(std::int32_t sample,
                                  PcmSampleFormat format) noexcept {
    if (format == PcmSampleFormat::kS16LE) {
      return sample >= -32768 && sample <= 32767;
    }
    return sample >= -8388608 && sample <= 8388607;
  }

  // 用流式线性插值把原生单声道样本转换到 16 kHz。同一速率直接搬运，保证 16 kHz
  // 路径逐样本不变；重采样保持相位和最后一个未消费样本，避免跨块不连续。
  void DrainResampler() {
    if (!format_known_) {
      return;
    }
    if (format_.sample_rate_hz == domain::kAudioSampleRateHz) {
      while (!native_samples_.empty()) {
        output_.push_back(native_samples_.front());
        native_samples_.pop_front();
      }
      native_base_sample_ = 0;
      next_output_position_ = 0.0;
      return;
    }

    const double ratio =
        static_cast<double>(format_.sample_rate_hz) / domain::kAudioSampleRateHz;
    while (true) {
      const double index_as_double = std::floor(next_output_position_);
      if (index_as_double + 1.0 >=
          static_cast<double>(native_base_sample_ + native_samples_.size())) {
        break;
      }
      const std::size_t index = static_cast<std::size_t>(
          index_as_double - static_cast<double>(native_base_sample_));
      const double fraction = next_output_position_ - index_as_double;
      const double interpolated =
          (1.0 - fraction) * static_cast<double>(native_samples_[index]) +
          fraction * static_cast<double>(native_samples_[index + 1]);
      const long rounded = std::lround(interpolated);
      const long clamped = std::clamp(rounded, -32768L, 32767L);
      output_.push_back(static_cast<std::int16_t>(clamped));
      next_output_position_ += ratio;
    }

    const std::uint64_t drop_end =
        static_cast<std::uint64_t>(std::floor(next_output_position_));
    while (native_base_sample_ < drop_end && !native_samples_.empty()) {
      native_samples_.pop_front();
      ++native_base_sample_;
    }
  }

  std::deque<std::int16_t> native_samples_;
  std::deque<std::int16_t> output_;
  std::uint64_t native_base_sample_ = 0;
  double next_output_position_ = 0.0;
  bool format_known_ = false;
  PcmCaptureFormat format_{};
};

std::string DeviceFailureMessage(domain::ErrorCode code, const std::string& detail) {
  if (detail.empty()) {
    return "ALSA 采集设备操作失败";
  }
  (void)code;
  return detail;
}

}  // namespace

domain::OperationResult validate_pcm_capture_backend_config(
    const PcmCaptureBackendConfig& config) {
  if (config.device.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "采集设备名不能为空");
  }
  if (config.sample_rate_hz == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "采集采样率必须大于 0");
  }
  if (config.channels == 0 || config.channels > 2) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "采集声道数只支持 1 或 2");
  }
  if (config.period_frames == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "period_frames 必须大于 0");
  }
  if (config.buffer_frames < config.period_frames) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "buffer_frames 不能小于 period_frames");
  }
  if (config.read_poll_slice_ms == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "read_poll_slice_ms 必须大于 0");
  }
  return OperationResult::success();
}

domain::OperationResult validate_alsa_audio_source_config(
    const AlsaAudioSourceConfig& config) {
  if (config.read_timeout.count() <= 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "read_timeout 必须大于 0");
  }
  if (config.max_backend_reads_per_frame == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "max_backend_reads_per_frame 必须大于 0");
  }
  return OperationResult::success();
}

struct AlsaAudioSource::Impl {
  AlsaAudioSourceConfig config_;
  std::unique_ptr<PcmCaptureBackend> backend_;

  // 状态互斥量保护 opened_/closed_/cancelled_/closing_、转换缓存和统计；设备互斥量
  // 串行化 backend_ 的 open/read/recover/close，保证 close 不会在 read 仍使用句柄时
  // 释放设备。cancel 不获取设备互斥量，只调用后端取消以有限唤醒阻塞读。
  mutable std::mutex state_mutex_;
  std::mutex device_mutex_;

  PcmFrameConverter converter_;
  std::deque<std::int16_t> output_pending_;
  bool opened_ = false;
  bool closed_ = false;
  bool cancelled_ = false;
  bool closing_ = false;
  AlsaAudioSourceStats stats_;

  Impl(AlsaAudioSourceConfig config, std::unique_ptr<PcmCaptureBackend> backend)
      : config_(std::move(config)), backend_(std::move(backend)) {}

  OperationResult open() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      stats_ = AlsaAudioSourceStats{};
      ++stats_.open_attempts;
      if (opened_ && !closed_) {
        return OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                        "音频输入已经打开");
      }
      if (!opened_ && cancelled_) {
        return OperationResult::failure(ErrorCode::kCancelled,
                                        "音频输入已经取消，需要先 close");
      }
      if (!backend_) {
        return OperationResult::failure(ErrorCode::kInvalidInput,
                                        "未提供 PCM 采集后端");
      }
      // 开始新一次打开尝试时清除上一轮的 closed_ 标记；后续成功路径只根据
      // closing_/cancelled_ 判断是否在本次打开期间收到收尾请求。打开失败时保持
      // opened_==false，close() 仍可幂等调用后端清理。
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
        opened_ = true;
        closed_ = false;
        cancelled_ = false;
        converter_.Reset();
        output_pending_.clear();
        ++stats_.open_successes;
      }
    }
    if (cancel_during_open) {
      // 若打开期间收到取消/关闭请求，立即在设备互斥量保护下回收句柄，不能把
      // 半打开设备暴露给读取；open 与 cancel/close 并发不是承诺场景，但必须先
      // 释放 state_mutex_ 再取 device_mutex_，避免与 read 的锁顺序相反。
      std::lock_guard<std::mutex> device_lock(device_mutex_);
      (void)backend_->close();
      return OperationResult::failure(ErrorCode::kCancelled,
                                      "音频输入打开期间被取消");
    }
    const PcmCaptureFormat format = backend_->current_format();
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      stats_.actual_sample_rate_hz = format.sample_rate_hz;
      stats_.actual_channels = format.channels;
      stats_.actual_format = format.format;
    }
    return OperationResult::success();
  }

  Result<AudioFrame> read() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++stats_.read_calls;
      if (!opened_ || closed_ || closing_) {
        return Result<AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                           "音频输入尚未打开或已经关闭");
      }
      if (cancelled_) {
        ++stats_.cancelled_reads;
        return Result<AudioFrame>::failure(ErrorCode::kCancelled,
                                           "音频输入已经取消");
      }
    }

    const auto deadline =
        std::chrono::steady_clock::now() + config_.read_timeout;
    std::size_t attempts = 0;
    std::size_t recoveries = 0;
    std::size_t reconnects = 0;

    while (true) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!opened_ || closed_ || closing_) {
          return Result<AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                             "音频输入已经关闭");
        }
        if (cancelled_) {
          ++stats_.cancelled_reads;
          return Result<AudioFrame>::failure(ErrorCode::kCancelled,
                                             "音频输入已经取消");
        }
        if (converter_.output_ready()) {
          return PopFrameLocked();
        }
      }

      if (attempts >= config_.max_backend_reads_per_frame) {
        return Result<AudioFrame>::failure(ErrorCode::kTimeout,
                                           "后端读取次数超过预算");
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return Result<AudioFrame>::failure(ErrorCode::kTimeout,
                                           "音频读取等待超过预算");
      }
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      if (remaining.count() <= 0) {
        remaining = std::chrono::milliseconds(1);
      }

      PcmCaptureReadResult read_result;
      {
        std::lock_guard<std::mutex> device_lock(device_mutex_);
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          if (cancelled_) {
            ++stats_.cancelled_reads;
            return Result<AudioFrame>::failure(ErrorCode::kCancelled,
                                               "音频输入已经取消");
          }
          if (closed_ || closing_ || !opened_) {
            return Result<AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                               "音频输入已经关闭");
          }
        }
        read_result = backend_->read(remaining);
        ++attempts;
      }

      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++stats_.backend_reads;
        if (cancelled_) {
          ++stats_.cancelled_reads;
          return Result<AudioFrame>::failure(ErrorCode::kCancelled,
                                             "音频输入已经取消");
        }
        if (closed_ || closing_ || !opened_) {
          return Result<AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                             "音频输入已经关闭");
        }

        switch (read_result.kind) {
          case PcmCaptureReadKind::kData: {
            const auto appended = converter_.Append(read_result.block);
            if (!appended.ok()) {
              return Result<AudioFrame>::failure(appended.error.code,
                                                 appended.error.message);
            }
            stats_.output_samples += *appended.value;
            continue;
          }
          case PcmCaptureReadKind::kTimeout:
            ++stats_.backend_timeouts;
            return Result<AudioFrame>::failure(
                ErrorCode::kTimeout,
                DeviceFailureMessage(ErrorCode::kTimeout, read_result.error.message));
          case PcmCaptureReadKind::kRecovered:
            ++stats_.backend_recoveries;
            if (recoveries >= config_.max_recoveries_per_frame) {
              return Result<AudioFrame>::failure(
                  ErrorCode::kDeviceFailure,
                  "底层音频恢复次数超过预算");
            }
            ++recoveries;
            DiscardConverterCacheLocked();
            continue;
          case PcmCaptureReadKind::kCancelled:
            cancelled_ = true;
            DiscardConverterCacheLocked();
            ++stats_.cancelled_reads;
            return Result<AudioFrame>::failure(ErrorCode::kCancelled,
                                               "音频输入已经取消");
          case PcmCaptureReadKind::kAlreadyCompleted:
            return Result<AudioFrame>::failure(ErrorCode::kAlreadyCompleted,
                                               "音频输入已经结束");
          case PcmCaptureReadKind::kDeviceFailure:
            ++stats_.device_failures;
            break;
        }
      }

      if (read_result.kind != PcmCaptureReadKind::kDeviceFailure) {
        continue;
      }

      if (reconnects >= config_.reconnect_budget) {
        return Result<AudioFrame>::failure(
            ErrorCode::kDeviceFailure,
            DeviceFailureMessage(ErrorCode::kDeviceFailure,
                                 read_result.error.message));
      }

      ++reconnects;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++stats_.reconnect_attempts;
      }
      OperationResult recovered;
      {
        std::lock_guard<std::mutex> device_lock(device_mutex_);
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          if (cancelled_) {
            ++stats_.cancelled_reads;
            return Result<AudioFrame>::failure(ErrorCode::kCancelled,
                                               "音频输入已经取消");
          }
          if (closed_ || closing_ || !opened_) {
            return Result<AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                               "音频输入已经关闭");
          }
        }
        recovered = backend_->recover();
      }
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (cancelled_) {
          ++stats_.cancelled_reads;
          return Result<AudioFrame>::failure(ErrorCode::kCancelled,
                                             "音频输入已经取消");
        }
        if (closed_ || closing_ || !opened_) {
          return Result<AudioFrame>::failure(ErrorCode::kDeviceFailure,
                                             "音频输入已经关闭");
        }
        if (recovered.ok()) {
          ++stats_.reconnect_successes;
          DiscardConverterCacheLocked();
          continue;
        }
        if (recovered.error.cancelled()) {
          cancelled_ = true;
          return Result<AudioFrame>::failure(ErrorCode::kCancelled,
                                             recovered.error.message);
        }
        return Result<AudioFrame>::failure(
            recovered.error.code == ErrorCode::kNone ? ErrorCode::kDeviceFailure
                                                     : recovered.error.code,
            recovered.error.message.empty() ? "ALSA 设备重连失败"
                                            : recovered.error.message);
      }
    }
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
        DiscardConverterCacheLocked();
        should_cancel_backend = true;
      } else {
        return OperationResult::success();
      }
    }
    if (should_cancel_backend && backend_) {
      const auto cancelled = backend_->cancel();
      if (!cancelled.ok()) {
        return cancelled;
      }
    }
    return OperationResult::success();
  }

  OperationResult close() noexcept {
    bool must_close_backend = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++stats_.close_calls;
      if (closed_ && !opened_) {
        return OperationResult::success();
      }
      closing_ = true;
      closed_ = true;
      opened_ = false;
      cancelled_ = false;
      DiscardConverterCacheLocked();
      must_close_backend = backend_ != nullptr;
    }

    OperationResult cancel_result;
    if (backend_ != nullptr) {
      cancel_result = backend_->cancel();
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
    }
    if (!cancel_result.ok()) {
      return cancel_result;
    }
    return close_result;
  }

  Result<AudioFrame> PopFrameLocked() {
    AudioFrame frame;
    frame.sample_rate_hz = domain::kAudioSampleRateHz;
    frame.channels = domain::kAudioChannels;
    frame.format = domain::AudioSampleFormat::kS16LE;
    converter_.PopFrame(&frame.samples);
    const auto valid = domain::validate_audio_frame(frame);
    if (!valid.ok()) {
      return Result<AudioFrame>::failure(ErrorCode::kBackendFailure,
                                         "转换器产出了不符合统一音频契约的帧");
    }
    ++stats_.frames_returned;
    return Result<AudioFrame>::success(std::move(frame));
  }

  void DiscardConverterCacheLocked() {
    stats_.samples_discarded_on_recovery +=
        converter_.pending_native_samples() + converter_.output_pending() +
        output_pending_.size();
    output_pending_.clear();
    converter_.Reset();
  }

  PcmCaptureFormat actual_format() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!opened_ || backend_ == nullptr) {
      return PcmCaptureFormat{};
    }
    return backend_->current_format();
  }

  AlsaAudioSourceStats stats() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return stats_;
  }

};

AlsaAudioSource::AlsaAudioSource(AlsaAudioSourceConfig config,
                                 std::unique_ptr<PcmCaptureBackend> backend)
    : impl_(new Impl(std::move(config), std::move(backend))) {}

AlsaAudioSource::~AlsaAudioSource() {
  if (impl_ != nullptr) {
    (void)impl_->close();
  }
}

domain::OperationResult AlsaAudioSource::open() {
  if (impl_ == nullptr) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "适配器未初始化");
  }
  const auto config_valid = validate_alsa_audio_source_config(impl_->config_);
  if (!config_valid.ok()) {
    return config_valid;
  }
  return impl_->open();
}

domain::Result<domain::AudioFrame> AlsaAudioSource::read() {
  if (impl_ == nullptr) {
    return domain::Result<domain::AudioFrame>::failure(ErrorCode::kInvalidInput,
                                                       "适配器未初始化");
  }
  return impl_->read();
}

domain::OperationResult AlsaAudioSource::cancel() noexcept {
  if (impl_ == nullptr) {
    return OperationResult::success();
  }
  return impl_->cancel();
}

domain::OperationResult AlsaAudioSource::close() noexcept {
  if (impl_ == nullptr) {
    return OperationResult::success();
  }
  return impl_->close();
}

PcmCaptureFormat AlsaAudioSource::actual_capture_format() const noexcept {
  if (impl_ == nullptr) {
    return PcmCaptureFormat{};
  }
  return impl_->actual_format();
}

AlsaAudioSourceStats AlsaAudioSource::stats() const noexcept {
  if (impl_ == nullptr) {
    return AlsaAudioSourceStats{};
  }
  return impl_->stats();
}

}  // namespace nexweave::backend

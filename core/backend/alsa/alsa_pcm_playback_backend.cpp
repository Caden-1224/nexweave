#include "alsa_pcm_playback_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <alsa/asoundlib.h>

namespace nexweave::backend {
namespace {

using domain::ErrorCode;
using domain::OperationResult;

ErrorCode MapAlsaError(int error) noexcept {
  switch (error) {
    case -EBUSY:
      return ErrorCode::kBusy;
    case -ENOENT:
    case -ENODEV:
    case -ENXIO:
    case -EACCES:
    case -EINVAL:
      return ErrorCode::kDeviceFailure;
    default:
      return ErrorCode::kDeviceFailure;
  }
}

std::string AlsaFailureMessage(const std::string& operation, int error) {
  return operation + " 失败: " + std::string(snd_strerror(error));
}

std::chrono::milliseconds RemainingSlice(std::chrono::steady_clock::time_point deadline,
                                         std::chrono::milliseconds slice) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds(1);
  }
  auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  if (remaining.count() <= 0) {
    return std::chrono::milliseconds(1);
  }
  return std::min(remaining, slice);
}

}  // namespace

struct AlsaPcmPlaybackBackend::Impl {
  explicit Impl(PcmPlaybackBackendConfig config) : config_(std::move(config)) {}

  ~Impl() {
    std::lock_guard<std::mutex> lock(mutex_);
    ClosePcmLocked();
  }

  OperationResult OpenDeviceLocked() {
    ClosePcmLocked();
    actual_format_ = PcmPlaybackFormat{};

    snd_pcm_t* pcm = nullptr;
    int error = snd_pcm_open(&pcm, config_.device.c_str(), SND_PCM_STREAM_PLAYBACK,
                             SND_PCM_NONBLOCK);
    if (error < 0) {
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("打开 ALSA 播放设备", error));
    }

    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    error = snd_pcm_hw_params_any(pcm, params);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("读取 ALSA 播放硬件参数", error));
    }

    error = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("设置 ALSA 播放访问方式", error));
    }

    const snd_pcm_format_t format = config_.format == PlaybackSampleFormat::kS16LE
                                        ? SND_PCM_FORMAT_S16_LE
                                        : SND_PCM_FORMAT_S24_LE;
    error = snd_pcm_hw_params_set_format(pcm, params, format);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          ErrorCode::kDeviceFailure,
          AlsaFailureMessage("协商 ALSA 播放样本格式（设备可能不支持该原生格式）", error));
    }

    error = snd_pcm_hw_params_set_channels(pcm, params, config_.channels);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          ErrorCode::kDeviceFailure,
          AlsaFailureMessage("协商 ALSA 播放声道数（设备可能不支持该配置）", error));
    }

    unsigned int requested_rate = config_.sample_rate_hz;
    int direction = 0;
    error = snd_pcm_hw_params_set_rate_near(pcm, params, &requested_rate, &direction);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          ErrorCode::kDeviceFailure, AlsaFailureMessage("协商 ALSA 播放采样率", error));
    }

    snd_pcm_uframes_t requested_period = config_.period_frames;
    error = snd_pcm_hw_params_set_period_size_near(pcm, params, &requested_period,
                                                   &direction);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          ErrorCode::kDeviceFailure, AlsaFailureMessage("协商 ALSA 播放 period", error));
    }

    snd_pcm_uframes_t requested_buffer = config_.buffer_frames;
    error = snd_pcm_hw_params_set_buffer_size_near(pcm, params, &requested_buffer);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          ErrorCode::kDeviceFailure, AlsaFailureMessage("协商 ALSA 播放 buffer", error));
    }

    error = snd_pcm_hw_params(pcm, params);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          ErrorCode::kDeviceFailure, AlsaFailureMessage("提交 ALSA 播放硬件参数", error));
    }

    unsigned int actual_rate = 0;
    unsigned int actual_channels = 0;
    direction = 0;
    error = snd_pcm_hw_params_get_rate(params, &actual_rate, &direction);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("读取实际播放采样率", error));
    }
    error = snd_pcm_hw_params_get_channels(params, &actual_channels);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("读取实际播放声道数", error));
    }
    snd_pcm_uframes_t actual_period = 0;
    error = snd_pcm_hw_params_get_period_size(params, &actual_period, &direction);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("读取实际播放 period", error));
    }
    snd_pcm_uframes_t actual_buffer = 0;
    error = snd_pcm_hw_params_get_buffer_size(params, &actual_buffer);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("读取实际播放 buffer", error));
    }

    error = snd_pcm_prepare(pcm);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("准备 ALSA 播放流", error));
    }

    pcm_ = pcm;
    actual_period_frames_ = static_cast<std::size_t>(actual_period);
    actual_buffer_frames_ = static_cast<std::size_t>(actual_buffer);
    actual_format_.sample_rate_hz = actual_rate;
    actual_format_.channels = static_cast<std::uint16_t>(actual_channels);
    actual_format_.format = config_.format;
    return OperationResult::success();
  }

  void ClosePcmLocked() noexcept {
    if (pcm_ == nullptr) {
      return;
    }
    (void)snd_pcm_drop(pcm_);
    (void)snd_pcm_close(pcm_);
    pcm_ = nullptr;
    actual_period_frames_ = 0;
    actual_buffer_frames_ = 0;
  }

  OperationResult RecoverStreamLocked(int error) {
    if (pcm_ == nullptr) {
      return OperationResult::failure(ErrorCode::kDeviceFailure,
                                      "ALSA 播放设备未打开");
    }
    const int recovered = snd_pcm_recover(pcm_, error, 1);
    if (recovered < 0) {
      return OperationResult::failure(
          MapAlsaError(recovered), AlsaFailureMessage("恢复 ALSA 播放流", recovered));
    }
    const int prepared = snd_pcm_prepare(pcm_);
    if (prepared < 0) {
      return OperationResult::failure(
          MapAlsaError(prepared), AlsaFailureMessage("重新准备 ALSA 播放流", prepared));
    }
    return OperationResult::success();
  }

  template <typename Sample>
  PcmPlaybackWriteResult WritePreparedLocked(const std::vector<Sample>& raw,
                                             std::size_t total_frames,
                                             std::chrono::milliseconds timeout) {
    const std::size_t channels = actual_format_.channels;
    std::size_t offset_frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (offset_frames < total_frames) {
      if (cancel_requested_.load()) {
        return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kCancelled,
                                              ErrorCode::kCancelled,
                                              "ALSA 播放已取消");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kTimeout,
                                              ErrorCode::kTimeout,
                                              "ALSA 播放写入超时");
      }

      const std::size_t remaining_frames = total_frames - offset_frames;
      const Sample* samples = raw.data() + offset_frames * channels;
      const snd_pcm_sframes_t written =
          snd_pcm_writei(pcm_, samples, static_cast<snd_pcm_uframes_t>(remaining_frames));
      if (written == -EAGAIN) {
        const auto slice = RemainingSlice(
            deadline, std::chrono::milliseconds(config_.write_poll_slice_ms));
        const int waited = snd_pcm_wait(pcm_, static_cast<int>(slice.count()));
        if (waited < 0) {
          if (waited == -EPIPE || waited == -ESTRPIPE) {
            const auto recovered = RecoverStreamLocked(waited);
            if (recovered.ok()) {
              return PcmPlaybackWriteResult::Status(
                  PcmPlaybackWriteKind::kRecovered, ErrorCode::kNone,
                  "ALSA 播放流已恢复");
            }
            return PcmPlaybackWriteResult::Status(
                PcmPlaybackWriteKind::kDeviceFailure, recovered.error.code,
                recovered.error.message);
          }
          return PcmPlaybackWriteResult::Status(
              PcmPlaybackWriteKind::kDeviceFailure, MapAlsaError(waited),
              AlsaFailureMessage("等待 ALSA 播放可写空间", waited));
        }
        if (waited == 0) {
          return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kTimeout,
                                                ErrorCode::kTimeout,
                                                "ALSA 播放没有在等待窗口内可写");
        }
        continue;
      }
      if (written == -EPIPE || written == -ESTRPIPE) {
        const auto recovered = RecoverStreamLocked(static_cast<int>(written));
        if (recovered.ok()) {
          return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kRecovered,
                                                ErrorCode::kNone,
                                                "ALSA 播放流已恢复");
        }
        return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kDeviceFailure,
                                              recovered.error.code,
                                              recovered.error.message);
      }
      if (written < 0) {
        return PcmPlaybackWriteResult::Status(
            PcmPlaybackWriteKind::kDeviceFailure, MapAlsaError(static_cast<int>(written)),
            AlsaFailureMessage("ALSA 播放写入", static_cast<int>(written)));
      }
      if (written == 0) {
        return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kTimeout,
                                              ErrorCode::kTimeout,
                                              "ALSA 播放写入返回 0 帧");
      }
      snd_pcm_sframes_t delay = -1;
      if (snd_pcm_delay(pcm_, &delay) < 0) {
        delay = -1;
      }
      const std::size_t accepted = static_cast<std::size_t>(written);
      if (accepted < remaining_frames) {
        ++stats_.short_writes;
      }
      return PcmPlaybackWriteResult::Written(accepted, static_cast<std::int64_t>(delay));
    }
    return PcmPlaybackWriteResult::Written(total_frames, -1);
  }

  PcmPlaybackBackendConfig config_;
  mutable std::mutex mutex_;
  snd_pcm_t* pcm_ = nullptr;
  PcmPlaybackFormat actual_format_{};
  std::size_t actual_period_frames_ = 0;
  std::size_t actual_buffer_frames_ = 0;
  std::atomic<bool> cancel_requested_{false};
  PcmPlaybackBackendStats stats_{};
};

AlsaPcmPlaybackBackend::AlsaPcmPlaybackBackend(PcmPlaybackBackendConfig config)
    : impl_(new Impl(std::move(config))) {}

AlsaPcmPlaybackBackend::~AlsaPcmPlaybackBackend() = default;

domain::OperationResult AlsaPcmPlaybackBackend::open() {
  const auto valid = validate_pcm_playback_backend_config(impl_->config_);
  if (!valid.ok()) {
    return valid;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  ++impl_->stats_.open_attempts;
  if (impl_->pcm_ != nullptr) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                    "ALSA 播放设备已经打开");
  }
  impl_->cancel_requested_.store(false);
  const auto opened = impl_->OpenDeviceLocked();
  if (!opened.ok()) {
    return opened;
  }
  ++impl_->stats_.open_successes;
  impl_->stats_.actual_sample_rate_hz = impl_->actual_format_.sample_rate_hz;
  impl_->stats_.actual_channels = impl_->actual_format_.channels;
  impl_->stats_.actual_format = impl_->actual_format_.format;
  impl_->stats_.actual_period_frames = impl_->actual_period_frames_;
  impl_->stats_.actual_buffer_frames = impl_->actual_buffer_frames_;
  return OperationResult::success();
}

PcmPlaybackWriteResult AlsaPcmPlaybackBackend::write(
    const std::int32_t* interleaved_samples, std::size_t frames,
    std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) {
    timeout = std::chrono::milliseconds(1);
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  ++impl_->stats_.write_calls;
  if (impl_->pcm_ == nullptr) {
    return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kDeviceFailure,
                                          ErrorCode::kDeviceFailure,
                                          "ALSA 播放设备尚未打开");
  }
  if (impl_->cancel_requested_.load()) {
    return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kCancelled,
                                          ErrorCode::kCancelled,
                                          "ALSA 播放已取消");
  }
  const std::size_t channels = impl_->actual_format_.channels;
  if (channels == 0 || interleaved_samples == nullptr) {
    return PcmPlaybackWriteResult::Status(PcmPlaybackWriteKind::kDeviceFailure,
                                          ErrorCode::kDeviceFailure,
                                          "ALSA 播放参数无效");
  }

  PcmPlaybackWriteResult result;
  const std::size_t sample_count = frames * channels;
  if (impl_->actual_format_.format == PlaybackSampleFormat::kS16LE) {
    std::vector<std::int16_t> raw(sample_count);
    for (std::size_t index = 0; index < sample_count; ++index) {
      raw[index] = static_cast<std::int16_t>(
          std::clamp(interleaved_samples[index], -32768, 32767));
    }
    result = impl_->WritePreparedLocked(raw, frames, timeout);
  } else {
    std::vector<std::uint32_t> raw(sample_count);
    for (std::size_t index = 0; index < sample_count; ++index) {
      const std::int32_t clamped =
          std::clamp(interleaved_samples[index], -8388608, 8388607);
      raw[index] = static_cast<std::uint32_t>(clamped);
    }
    result = impl_->WritePreparedLocked(raw, frames, timeout);
  }
  if (result.kind == PcmPlaybackWriteKind::kWritten) {
    ++impl_->stats_.written_calls;
  } else if (result.kind == PcmPlaybackWriteKind::kRecovered) {
    ++impl_->stats_.recovered_calls;
  } else if (result.kind == PcmPlaybackWriteKind::kTimeout) {
    ++impl_->stats_.timeout_calls;
  } else if (result.kind == PcmPlaybackWriteKind::kDeviceFailure) {
    ++impl_->stats_.device_failures;
  }
  return result;
}

domain::OperationResult AlsaPcmPlaybackBackend::recover() {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  ++impl_->stats_.recover_attempts;
  if (impl_->cancel_requested_.load()) {
    return OperationResult::failure(ErrorCode::kCancelled,
                                    "ALSA 播放恢复前已经取消");
  }
  const auto reopened = impl_->OpenDeviceLocked();
  if (!reopened.ok()) {
    return reopened;
  }
  if (impl_->cancel_requested_.load()) {
    impl_->ClosePcmLocked();
    return OperationResult::failure(ErrorCode::kCancelled,
                                    "ALSA 播放恢复期间已经取消");
  }
  ++impl_->stats_.recover_successes;
  impl_->stats_.actual_sample_rate_hz = impl_->actual_format_.sample_rate_hz;
  impl_->stats_.actual_channels = impl_->actual_format_.channels;
  impl_->stats_.actual_format = impl_->actual_format_.format;
  impl_->stats_.actual_period_frames = impl_->actual_period_frames_;
  impl_->stats_.actual_buffer_frames = impl_->actual_buffer_frames_;
  return OperationResult::success();
}

domain::OperationResult AlsaPcmPlaybackBackend::drain(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) {
    timeout = std::chrono::milliseconds(1);
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  ++impl_->stats_.drain_calls;
  if (impl_->pcm_ == nullptr) {
    return OperationResult::success();
  }
  const snd_pcm_state_t state = snd_pcm_state(impl_->pcm_);
  if (state != SND_PCM_STATE_RUNNING && state != SND_PCM_STATE_DRAINING) {
    return OperationResult::success();
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (impl_->cancel_requested_.load()) {
      return OperationResult::failure(ErrorCode::kCancelled,
                                      "ALSA 播放排空前已经取消");
    }
    snd_pcm_sframes_t delay = 0;
    const int error = snd_pcm_delay(impl_->pcm_, &delay);
    if (error == -EPIPE || error == -ESTRPIPE) {
      // 数据全部播完后播放流进入正常 underrun，设备缓冲已空；此时 drain 的目标
      // 已经达成，不能再把它报告成失败，否则正常 close 会被误判为设备故障。
      return OperationResult::success();
    }
    if (error < 0) {
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("查询 ALSA 播放延迟", error));
    }
    if (delay <= 0) {
      return OperationResult::success();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      ++impl_->stats_.drain_timeouts;
      return OperationResult::failure(ErrorCode::kTimeout,
                                      "ALSA 播放排空等待超时");
    }
    auto slice = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto configured_slice =
        std::chrono::milliseconds(impl_->config_.drain_poll_slice_ms);
    if (slice > configured_slice) {
      slice = configured_slice;
    }
    if (slice.count() <= 0) {
      slice = std::chrono::milliseconds(1);
    }
    std::this_thread::sleep_for(slice);
  }
}

domain::OperationResult AlsaPcmPlaybackBackend::cancel() noexcept {
  impl_->cancel_requested_.store(true);
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  ++impl_->stats_.cancelled_calls;
  if (impl_->pcm_ != nullptr) {
    (void)snd_pcm_drop(impl_->pcm_);
    (void)snd_pcm_prepare(impl_->pcm_);
  }
  return OperationResult::success();
}

domain::OperationResult AlsaPcmPlaybackBackend::close() noexcept {
  impl_->cancel_requested_.store(true);
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  ++impl_->stats_.close_calls;
  impl_->ClosePcmLocked();
  impl_->actual_format_ = PcmPlaybackFormat{};
  return OperationResult::success();
}

PcmPlaybackFormat AlsaPcmPlaybackBackend::current_format() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->actual_format_;
}

PcmPlaybackBackendStats AlsaPcmPlaybackBackend::stats() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->stats_;
}

}  // namespace nexweave::backend

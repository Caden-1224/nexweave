#include "alsa_pcm_capture_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <alsa/asoundlib.h>

namespace nexweave::backend {
namespace {

using domain::ErrorCode;
using domain::OperationResult;

// 把 ALSA 负数错误码转换为本项目错误码。格式/参数不支持归为设备初始化失败，
// 因为调用方已经通过显式配置表达请求；是否修正配置由调用方根据消息决定。
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

}  // namespace

struct AlsaPcmCaptureBackend::Impl {
  explicit Impl(PcmCaptureBackendConfig config) : config_(std::move(config)) {}

  ~Impl() {
    std::lock_guard<std::mutex> lock(mutex_);
    ClosePcmLocked();
  }

  // 打开设备并完成显式格式协商。调用方必须已持有 mutex_；错误返回时保证
  // pcm_ 为空且 actual_format_ 无效，不会留下半配置句柄。
  OperationResult OpenDeviceLocked() {
    ClosePcmLocked();
    actual_format_ = PcmCaptureFormat{};

    snd_pcm_t* pcm = nullptr;
    int error = snd_pcm_open(&pcm, config_.device.c_str(), SND_PCM_STREAM_CAPTURE,
                             SND_PCM_NONBLOCK);
    if (error < 0) {
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("打开 ALSA 采集设备", error));
    }

    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    error = snd_pcm_hw_params_any(pcm, params);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("读取 ALSA 硬件参数", error));
    }

    error = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("设置 ALSA 访问方式", error));
    }

    const snd_pcm_format_t format = config_.format == PcmSampleFormat::kS16LE
                                        ? SND_PCM_FORMAT_S16_LE
                                        : SND_PCM_FORMAT_S24_LE;
    error = snd_pcm_hw_params_set_format(pcm, params, format);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          ErrorCode::kDeviceFailure,
          AlsaFailureMessage("协商 ALSA 样本格式（设备可能不支持该原生格式）", error));
    }

    error = snd_pcm_hw_params_set_channels(pcm, params, config_.channels);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          ErrorCode::kDeviceFailure,
          AlsaFailureMessage("协商 ALSA 声道数（设备可能不支持该配置）", error));
    }

    unsigned int requested_rate = config_.sample_rate_hz;
    int direction = 0;
    error = snd_pcm_hw_params_set_rate_near(pcm, params, &requested_rate, &direction);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          ErrorCode::kDeviceFailure,
          AlsaFailureMessage("协商 ALSA 采样率", error));
    }

    snd_pcm_uframes_t requested_period = config_.period_frames;
    error = snd_pcm_hw_params_set_period_size_near(pcm, params, &requested_period,
                                                   &direction);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          ErrorCode::kDeviceFailure,
          AlsaFailureMessage("协商 ALSA period 帧数", error));
    }

    snd_pcm_uframes_t requested_buffer = config_.buffer_frames;
    error = snd_pcm_hw_params_set_buffer_size_near(pcm, params, &requested_buffer);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          ErrorCode::kDeviceFailure,
          AlsaFailureMessage("协商 ALSA buffer 帧数", error));
    }

    error = snd_pcm_hw_params(pcm, params);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          ErrorCode::kDeviceFailure,
          AlsaFailureMessage("提交 ALSA 硬件参数", error));
    }

    unsigned int actual_rate = 0;
    unsigned int actual_channels = 0;
    direction = 0;
    error = snd_pcm_hw_params_get_rate(params, &actual_rate, &direction);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("读取实际采样率", error));
    }
    error = snd_pcm_hw_params_get_channels(params, &actual_channels);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("读取实际声道数", error));
    }

    snd_pcm_uframes_t actual_period = 0;
    error = snd_pcm_hw_params_get_period_size(params, &actual_period, &direction);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("读取实际 period 帧数", error));
    }
    snd_pcm_uframes_t actual_buffer = 0;
    error = snd_pcm_hw_params_get_buffer_size(params, &actual_buffer);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("读取实际 buffer 帧数", error));
    }

    error = snd_pcm_prepare(pcm);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("准备 ALSA 采集流", error));
    }
    error = snd_pcm_start(pcm);
    if (error < 0) {
      snd_pcm_close(pcm);
      return OperationResult::failure(
          MapAlsaError(error), AlsaFailureMessage("启动 ALSA 采集流", error));
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

  // 处理 overrun/underrun/挂起等可恢复错误：先走 ALSA 恢复，再按 capture 语义
  // 重新 prepare/start。返回成功后调用方需要丢弃转换缓存后重试。
  OperationResult RecoverStreamLocked(int error) {
    if (pcm_ == nullptr) {
      return OperationResult::failure(ErrorCode::kDeviceFailure,
                                      "ALSA 采集设备未打开");
    }
    const int recovered = snd_pcm_recover(pcm_, error, 1);
    if (recovered < 0) {
      return OperationResult::failure(
          MapAlsaError(recovered), AlsaFailureMessage("恢复 ALSA 采集流", recovered));
    }
    int prepared = snd_pcm_prepare(pcm_);
    if (prepared >= 0) {
      prepared = snd_pcm_start(pcm_);
    }
    if (prepared < 0) {
      return OperationResult::failure(
          MapAlsaError(prepared), AlsaFailureMessage("重启 ALSA 采集流", prepared));
    }
    return OperationResult::success();
  }

  PcmCaptureReadResult ReadAvailableLocked() {
    const std::size_t requested_frames =
        actual_period_frames_ == 0 ? config_.period_frames : actual_period_frames_;
    const std::size_t channels = actual_format_.channels;
    if (requested_frames == 0 || channels == 0) {
      return PcmCaptureReadResult::Status(PcmCaptureReadKind::kDeviceFailure,
                                          ErrorCode::kDeviceFailure,
                                          "ALSA 实际参数无效");
    }

    PcmCaptureBlock block;
    block.format = actual_format_;
    if (actual_format_.format == PcmSampleFormat::kS16LE) {
      std::vector<std::int16_t> raw(requested_frames * channels);
      const snd_pcm_sframes_t count =
          snd_pcm_readi(pcm_, raw.data(), static_cast<snd_pcm_uframes_t>(requested_frames));
      if (count == -EAGAIN) {
        ++stats_.timeout_blocks;
        return PcmCaptureReadResult::Status(PcmCaptureReadKind::kTimeout,
                                            ErrorCode::kTimeout,
                                            "ALSA 数据尚未就绪");
      }
      if (count == -EPIPE || count == -ESTRPIPE) {
        const auto recovered = RecoverStreamLocked(static_cast<int>(count));
        if (!recovered.ok()) {
          return PcmCaptureReadResult::Status(PcmCaptureReadKind::kDeviceFailure,
                                              recovered.error.code,
                                              recovered.error.message);
        }
        ++stats_.recovered_blocks;
        return PcmCaptureReadResult::Status(PcmCaptureReadKind::kRecovered,
                                            ErrorCode::kNone, "ALSA 采集流已恢复");
      }
      if (count < 0) {
        ++stats_.device_failures;
        return PcmCaptureReadResult::Status(
            PcmCaptureReadKind::kDeviceFailure, MapAlsaError(static_cast<int>(count)),
            AlsaFailureMessage("ALSA 读取采集数据", static_cast<int>(count)));
      }
      if (count == 0) {
        ++stats_.timeout_blocks;
        return PcmCaptureReadResult::Status(PcmCaptureReadKind::kTimeout,
                                            ErrorCode::kTimeout,
                                            "ALSA 本次没有返回样本");
      }
      const std::size_t frames = static_cast<std::size_t>(count);
      block.interleaved_samples.resize(frames * channels);
      for (std::size_t index = 0; index < block.interleaved_samples.size(); ++index) {
        block.interleaved_samples[index] = static_cast<std::int32_t>(raw[index]);
      }
      if (frames < requested_frames) {
        ++stats_.short_reads;
      }
      ++stats_.data_blocks;
      return PcmCaptureReadResult::Data(std::move(block));
    }

    // ALSA 的 SND_PCM_FORMAT_S24_LE 是 4 字节容器承载 24 位有效样本；
    // 紧凑 3 字节格式是 SND_PCM_FORMAT_S24_3LE，本适配器不请求后者。
    const std::size_t bytes_per_sample = 4U;
    const std::size_t bytes_per_frame = channels * bytes_per_sample;
    std::vector<std::uint8_t> raw(requested_frames * bytes_per_frame);
    const snd_pcm_sframes_t count =
        snd_pcm_readi(pcm_, raw.data(), static_cast<snd_pcm_uframes_t>(requested_frames));
    if (count == -EAGAIN) {
      ++stats_.timeout_blocks;
      return PcmCaptureReadResult::Status(PcmCaptureReadKind::kTimeout,
                                          ErrorCode::kTimeout,
                                          "ALSA 数据尚未就绪");
    }
    if (count == -EPIPE || count == -ESTRPIPE) {
      const auto recovered = RecoverStreamLocked(static_cast<int>(count));
      if (!recovered.ok()) {
        return PcmCaptureReadResult::Status(PcmCaptureReadKind::kDeviceFailure,
                                            recovered.error.code,
                                            recovered.error.message);
      }
      ++stats_.recovered_blocks;
      return PcmCaptureReadResult::Status(PcmCaptureReadKind::kRecovered,
                                          ErrorCode::kNone, "ALSA 采集流已恢复");
    }
    if (count < 0) {
      ++stats_.device_failures;
      return PcmCaptureReadResult::Status(
          PcmCaptureReadKind::kDeviceFailure, MapAlsaError(static_cast<int>(count)),
          AlsaFailureMessage("ALSA 读取采集数据", static_cast<int>(count)));
    }
    if (count == 0) {
      ++stats_.timeout_blocks;
      return PcmCaptureReadResult::Status(PcmCaptureReadKind::kTimeout,
                                          ErrorCode::kTimeout,
                                          "ALSA 本次没有返回样本");
    }
    const std::size_t frames = static_cast<std::size_t>(count);
    block.interleaved_samples.resize(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      for (std::size_t channel = 0; channel < channels; ++channel) {
        const std::size_t offset = (frame * channels + channel) * bytes_per_sample;
        const std::uint32_t word =
            static_cast<std::uint32_t>(raw[offset]) |
            (static_cast<std::uint32_t>(raw[offset + 1U]) << 8U) |
            (static_cast<std::uint32_t>(raw[offset + 2U]) << 16U) |
            (static_cast<std::uint32_t>(raw[offset + 3U]) << 24U);
        std::int32_t value = static_cast<std::int32_t>(word & 0x00FFFFFFU);
        if ((word & 0x00800000U) != 0U) {
          value |= ~0x00FFFFFF;
        }
        block.interleaved_samples[frame * channels + channel] = value;
      }
    }
    if (frames < requested_frames) {
      ++stats_.short_reads;
    }
    ++stats_.data_blocks;
    return PcmCaptureReadResult::Data(std::move(block));
  }

  PcmCaptureBackendConfig config_;
  mutable std::mutex mutex_;
  snd_pcm_t* pcm_ = nullptr;
  PcmCaptureFormat actual_format_{};
  std::size_t actual_period_frames_ = 0;
  std::size_t actual_buffer_frames_ = 0;
  std::atomic<bool> cancel_requested_{false};
  PcmCaptureBackendStats stats_{};
};

AlsaPcmCaptureBackend::AlsaPcmCaptureBackend(PcmCaptureBackendConfig config)
    : impl_(new Impl(std::move(config))) {}

AlsaPcmCaptureBackend::~AlsaPcmCaptureBackend() = default;

domain::OperationResult AlsaPcmCaptureBackend::open() {
  const auto valid = validate_pcm_capture_backend_config(impl_->config_);
  if (!valid.ok()) {
    return valid;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  ++impl_->stats_.open_attempts;
  if (impl_->pcm_ != nullptr) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                    "ALSA 采集设备已经打开");
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

PcmCaptureReadResult AlsaPcmCaptureBackend::read(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) {
    timeout = std::chrono::milliseconds(1);
  }
  std::unique_lock<std::mutex> lock(impl_->mutex_);
  ++impl_->stats_.read_calls;
  if (impl_->pcm_ == nullptr) {
    return PcmCaptureReadResult::Status(PcmCaptureReadKind::kDeviceFailure,
                                        ErrorCode::kDeviceFailure,
                                        "ALSA 采集设备尚未打开");
  }
  if (impl_->cancel_requested_.load()) {
    return PcmCaptureReadResult::Status(PcmCaptureReadKind::kCancelled,
                                        ErrorCode::kCancelled,
                                        "ALSA 采集已取消");
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (impl_->cancel_requested_.load()) {
      return PcmCaptureReadResult::Status(PcmCaptureReadKind::kCancelled,
                                          ErrorCode::kCancelled,
                                          "ALSA 采集已取消");
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      ++impl_->stats_.timeout_blocks;
      return PcmCaptureReadResult::Status(PcmCaptureReadKind::kTimeout,
                                          ErrorCode::kTimeout,
                                          "ALSA 采集等待超时");
    }

    auto slice = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto configured_slice = std::chrono::milliseconds(impl_->config_.read_poll_slice_ms);
    if (slice > configured_slice) {
      slice = configured_slice;
    }
    if (slice.count() <= 0) {
      slice = std::chrono::milliseconds(1);
    }

    const int wait_result = snd_pcm_wait(impl_->pcm_, static_cast<int>(slice.count()));
    if (impl_->cancel_requested_.load()) {
      return PcmCaptureReadResult::Status(PcmCaptureReadKind::kCancelled,
                                          ErrorCode::kCancelled,
                                          "ALSA 采集已取消");
    }
    if (wait_result < 0) {
      if (wait_result == -EINTR) {
        continue;
      }
      if (wait_result == -EPIPE || wait_result == -ESTRPIPE) {
        const auto recovered = impl_->RecoverStreamLocked(wait_result);
        if (recovered.ok()) {
          ++impl_->stats_.recovered_blocks;
          return PcmCaptureReadResult::Status(PcmCaptureReadKind::kRecovered,
                                              ErrorCode::kNone,
                                              "ALSA 采集流已恢复");
        }
        return PcmCaptureReadResult::Status(PcmCaptureReadKind::kDeviceFailure,
                                            recovered.error.code,
                                            recovered.error.message);
      }
      ++impl_->stats_.device_failures;
      return PcmCaptureReadResult::Status(
          PcmCaptureReadKind::kDeviceFailure, MapAlsaError(wait_result),
          AlsaFailureMessage("等待 ALSA 采集数据", wait_result));
    }
    if (wait_result == 0) {
      continue;
    }
    return impl_->ReadAvailableLocked();
  }
}

domain::OperationResult AlsaPcmCaptureBackend::recover() {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  ++impl_->stats_.recover_attempts;
  if (impl_->cancel_requested_.load()) {
    return OperationResult::failure(ErrorCode::kCancelled,
                                    "ALSA 采集恢复前已经取消");
  }
  const auto reopened = impl_->OpenDeviceLocked();
  if (!reopened.ok()) {
    return reopened;
  }
  if (impl_->cancel_requested_.load()) {
    impl_->ClosePcmLocked();
    return OperationResult::failure(ErrorCode::kCancelled,
                                    "ALSA 采集恢复期间已经取消");
  }
  ++impl_->stats_.recover_successes;
  impl_->stats_.actual_sample_rate_hz = impl_->actual_format_.sample_rate_hz;
  impl_->stats_.actual_channels = impl_->actual_format_.channels;
  impl_->stats_.actual_format = impl_->actual_format_.format;
  impl_->stats_.actual_period_frames = impl_->actual_period_frames_;
  impl_->stats_.actual_buffer_frames = impl_->actual_buffer_frames_;
  return OperationResult::success();
}

domain::OperationResult AlsaPcmCaptureBackend::cancel() noexcept {
  // 先写原子标志，让可能正持有 mutex_ 等待数据的 read 在下一个 poll slice 后退出；
  // 再获取互斥量记录计数。cancel 与 read 的线性化点就是这个原子写。
  impl_->cancel_requested_.store(true);
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  ++impl_->stats_.cancel_calls;
  return OperationResult::success();
}

domain::OperationResult AlsaPcmCaptureBackend::close() noexcept {
  // 先置原子取消标志，避免 close 等待 read 持有的 mutex_ 时阻塞整个 poll 窗口。
  impl_->cancel_requested_.store(true);
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  ++impl_->stats_.close_calls;
  impl_->ClosePcmLocked();
  impl_->actual_format_ = PcmCaptureFormat{};
  return OperationResult::success();
}

PcmCaptureFormat AlsaPcmCaptureBackend::current_format() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->actual_format_;
}

PcmCaptureBackendStats AlsaPcmCaptureBackend::stats() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->stats_;
}

}  // namespace nexweave::backend

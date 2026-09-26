#include "webrtc_audio_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <utility>

namespace nexweave::backend {
namespace {

using runtime::AudioProcessorConfig;
using runtime::AudioProcessorFrame;
using runtime::AudioProcessorState;
using runtime::AudioProcessorStats;

// 这里只声明板端库实际导出的旧版 WebRTC C ABI。声明来自公开 ABI 行为，
// 不复制第三方实现；字段/函数签名必须与链接库保持一致。
struct NsHandle;

struct AecLevel {
  int instant;
  int average;
  int max;
  int min;
};

struct AecMetrics {
  AecLevel rerl;
  AecLevel erl;
  AecLevel erle;
  AecLevel aNlp;
};

struct AecConfig {
  std::int16_t nlpMode;
  std::int16_t skewMode;
  std::int16_t metricsMode;
  int delay_logging;
};

constexpr int kAecNlpModerate = 1;
constexpr int kAecFalse = 0;
constexpr int kAecTrue = 1;

extern "C" {
void* WebRtcAec_Create();
void WebRtcAec_Free(void* aec_inst);
std::int32_t WebRtcAec_Init(void* aec_inst, std::int32_t sample_rate,
                            std::int32_t soundcard_rate);
std::int32_t WebRtcAec_BufferFarend(void* aec_inst, const float* farend,
                                    std::size_t samples);
std::int32_t WebRtcAec_Process(void* aec_inst, const float* const* nearend,
                               std::size_t num_bands, float* const* out,
                               std::size_t samples, std::int16_t delay_ms,
                               std::int32_t skew);
int WebRtcAec_set_config(void* handle, AecConfig config);
int WebRtcAec_GetMetrics(void* handle, AecMetrics* metrics);
int WebRtcAec_GetDelayMetrics(void* handle, int* median, int* std,
                              float* fraction_poor_delays);
std::int32_t WebRtcAec_get_error_code(void* aec_inst);

NsHandle* WebRtcNs_Create();
void WebRtcNs_Free(NsHandle* ns_inst);
int WebRtcNs_Init(NsHandle* ns_inst, std::uint32_t sample_rate);
int WebRtcNs_set_policy(NsHandle* ns_inst, int mode);
void WebRtcNs_Analyze(NsHandle* ns_inst, const float* frame);
void WebRtcNs_Process(NsHandle* ns_inst, const float* const* in,
                      std::size_t num_bands, float* const* out);
}

float Int16ToFloat(std::int16_t sample) {
  return static_cast<float>(sample) / 32768.0F;
}

std::int16_t FloatToInt16(float sample) {
  const float clamped = std::clamp(sample, -1.0F, 1.0F);
  const long rounded = std::lround(clamped * 32767.0F);
  return static_cast<std::int16_t>(std::clamp(rounded, -32768L, 32767L));
}

}  // namespace

struct WebrtcAudioProcessor::Impl {
  explicit Impl(AudioProcessorConfig config) : config_(config) {
    config_.noise_suppression_level =
        std::clamp(config_.noise_suppression_level, 0, 2);
    if (config_.min_delay_estimates_for_convergence == 0) {
      config_.min_delay_estimates_for_convergence = 1;
    }
  }

  domain::OperationResult InitializeLocked() {
    aec_ = WebRtcAec_Create();
    if (aec_ == nullptr) {
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                             "WebRTC AEC 句柄创建失败");
    }
    const std::int32_t init_result =
        WebRtcAec_Init(aec_, static_cast<std::int32_t>(16000),
                       static_cast<std::int32_t>(16000));
    if (init_result != 0) {
      WebRtcAec_Free(aec_);
      aec_ = nullptr;
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                             "WebRTC AEC 初始化失败");
    }
    AecConfig aec_config{};
    aec_config.nlpMode = kAecNlpModerate;
    aec_config.skewMode = kAecFalse;
    aec_config.metricsMode = kAecTrue;
    aec_config.delay_logging = kAecTrue;
    if (WebRtcAec_set_config(aec_, aec_config) != 0) {
      WebRtcAec_Free(aec_);
      aec_ = nullptr;
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                             "WebRTC AEC 配置失败");
    }

    ns_ = WebRtcNs_Create();
    if (ns_ == nullptr) {
      WebRtcAec_Free(aec_);
      aec_ = nullptr;
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                             "WebRTC 降噪句柄创建失败");
    }
    if (WebRtcNs_Init(ns_, 16000) != 0 ||
        WebRtcNs_set_policy(ns_, config_.noise_suppression_level) != 0) {
      WebRtcNs_Free(ns_);
      WebRtcAec_Free(aec_);
      ns_ = nullptr;
      aec_ = nullptr;
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                             "WebRTC 降噪初始化失败");
    }
    initialized_ = true;
    delay_metrics_requested_ = false;
    stats_.initialized = true;
    stats_.state = AudioProcessorState::kNotConverged;
    stats_.render_frames_since_reset = 0;
    stats_.capture_frames_since_reset = 0;
    stats_.delay_measurements = 0;
    stats_.delay_median_ms = -1;
    stats_.delay_std_ms = -1;
    stats_.fraction_poor_delays = -1.0F;
    stats_.erle_average_db = 0;
    stats_.erl_average_db = 0;
    stats_.a_nlp_average = 0;
    return domain::OperationResult::success();
  }

  void ReleaseLocked() noexcept {
    if (ns_ != nullptr) {
      WebRtcNs_Free(ns_);
      ns_ = nullptr;
    }
    if (aec_ != nullptr) {
      WebRtcAec_Free(aec_);
      aec_ = nullptr;
    }
    initialized_ = false;
    stats_.initialized = false;
    stats_.state = AudioProcessorState::kClosed;
  }

  void MarkFailedLocked(const std::string& message) {
    stats_.state = AudioProcessorState::kFailed;
    stats_.last_reset_reason = message;
  }

  void UpdateMetricsLocked() {
    AecMetrics metrics{};
    if (WebRtcAec_GetMetrics(aec_, &metrics) == 0) {
      stats_.erle_average_db = metrics.erle.average;
      stats_.erl_average_db = metrics.erl.average;
      stats_.a_nlp_average = metrics.aNlp.average;
    }
    // 旧版 AEC 的 GetDelayMetrics 在一次查询后就把结果标记为已交付；如果在
    // 延迟估计尚未积累时提前查询，会缓存 -1 且不再刷新。因此只在配置要求的
    // 初始化窗口之后查询一次，让第一次查询落在 delay 直方图已有数据的时刻。
    if (!delay_metrics_requested_ &&
        stats_.capture_frames_since_reset >=
            config_.min_capture_frames_for_convergence) {
      delay_metrics_requested_ = true;
      int median = -1;
      int standard_deviation = -1;
      float poor_fraction = -1.0F;
      if (WebRtcAec_GetDelayMetrics(aec_, &median, &standard_deviation,
                                    &poor_fraction) == 0) {
        stats_.delay_median_ms = median;
        stats_.delay_std_ms = standard_deviation;
        stats_.fraction_poor_delays = poor_fraction;
        ++stats_.delay_measurements;
      }
    }
  }

  void UpdateConvergenceLocked() {
    if (!initialized_ || stats_.state == AudioProcessorState::kFailed) {
      return;
    }
    if (stats_.render_frames_since_reset <
            config_.min_render_frames_for_convergence ||
        stats_.capture_frames_since_reset <
            config_.min_capture_frames_for_convergence) {
      stats_.state = AudioProcessorState::kNotConverged;
      return;
    }
    if (stats_.delay_measurements <
        config_.min_delay_estimates_for_convergence) {
      stats_.state = AudioProcessorState::kConverging;
      return;
    }
    if (stats_.fraction_poor_delays < 0.0F ||
        stats_.fraction_poor_delays > config_.max_poor_delay_fraction) {
      stats_.state = AudioProcessorState::kConverging;
      return;
    }
    if (config_.require_erle_for_convergence &&
        stats_.erle_average_db < config_.min_erle_average_db) {
      stats_.state = AudioProcessorState::kConverging;
      return;
    }
    stats_.state = AudioProcessorState::kConverged;
  }

  AudioProcessorConfig config_;
  mutable std::mutex mutex_;
  void* aec_ = nullptr;
  NsHandle* ns_ = nullptr;
  bool initialized_ = false;
  bool delay_metrics_requested_ = false;
  std::array<float, runtime::kAudioProcessorFrameSamples> render_floats_{};
  std::array<float, runtime::kAudioProcessorFrameSamples> capture_floats_{};
  const float* capture_ptrs_[1] = {capture_floats_.data()};
  float* capture_out_ptrs_[1] = {capture_floats_.data()};
  AudioProcessorStats stats_;
};

WebrtcAudioProcessor::WebrtcAudioProcessor(AudioProcessorConfig config)
    : impl_(new Impl(std::move(config))) {}

WebrtcAudioProcessor::~WebrtcAudioProcessor() {
  if (impl_ != nullptr) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->ReleaseLocked();
  }
}

domain::OperationResult WebrtcAudioProcessor::open() {
  if (impl_ == nullptr) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                           "WebRTC 前处理器未初始化");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  if (impl_->initialized_) {
    return domain::OperationResult::failure(domain::ErrorCode::kAlreadyCompleted,
                                           "WebRTC 前处理器已经打开");
  }
  return impl_->InitializeLocked();
}

domain::OperationResult WebrtcAudioProcessor::process_render(
    const AudioProcessorFrame& frame) {
  if (impl_ == nullptr) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                           "WebRTC 前处理器未初始化");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  if (!impl_->initialized_) {
    return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                           "WebRTC 前处理器尚未打开");
  }
  if (impl_->stats_.state == AudioProcessorState::kFailed) {
    return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                           "WebRTC 前处理器已经失败");
  }
  for (std::size_t i = 0; i < frame.size(); ++i) {
    impl_->render_floats_[i] = Int16ToFloat(frame[i]);
  }
  if (WebRtcAec_BufferFarend(impl_->aec_, impl_->render_floats_.data(),
                             frame.size()) != 0) {
    ++impl_->stats_.process_render_errors;
    impl_->MarkFailedLocked("WebRTC AEC 渲染参考处理失败");
    return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                           "WebRTC AEC 渲染参考处理失败");
  }
  ++impl_->stats_.render_frames;
  ++impl_->stats_.render_frames_since_reset;
  return domain::OperationResult::success();
}

domain::Result<AudioProcessorFrame> WebrtcAudioProcessor::process_capture(
    const AudioProcessorFrame& frame) {
  if (impl_ == nullptr) {
    return domain::Result<AudioProcessorFrame>::failure(
        domain::ErrorCode::kInvalidInput, "WebRTC 前处理器未初始化");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  if (!impl_->initialized_) {
    return domain::Result<AudioProcessorFrame>::failure(
        domain::ErrorCode::kDeviceFailure, "WebRTC 前处理器尚未打开");
  }
  if (impl_->stats_.state == AudioProcessorState::kFailed) {
    return domain::Result<AudioProcessorFrame>::failure(
        domain::ErrorCode::kBackendFailure, "WebRTC 前处理器已经失败");
  }
  for (std::size_t i = 0; i < frame.size(); ++i) {
    impl_->capture_floats_[i] = Int16ToFloat(frame[i]);
  }
  if (WebRtcAec_Process(impl_->aec_, impl_->capture_ptrs_, 1,
                        impl_->capture_out_ptrs_, frame.size(),
                        static_cast<std::int16_t>(
                            std::clamp(impl_->config_.stream_delay_ms, 0, 32767)),
                        0) != 0) {
    ++impl_->stats_.process_capture_errors;
    impl_->MarkFailedLocked("WebRTC AEC 近端处理失败");
    return domain::Result<AudioProcessorFrame>::failure(
        domain::ErrorCode::kBackendFailure, "WebRTC AEC 近端处理失败");
  }
  WebRtcNs_Analyze(impl_->ns_, impl_->capture_out_ptrs_[0]);
  const float* ns_input[1] = {impl_->capture_out_ptrs_[0]};
  WebRtcNs_Process(impl_->ns_, ns_input, 1, impl_->capture_out_ptrs_);
  AudioProcessorFrame output{};
  for (std::size_t i = 0; i < output.size(); ++i) {
    output[i] = FloatToInt16(impl_->capture_out_ptrs_[0][i]);
  }
  ++impl_->stats_.capture_frames;
  ++impl_->stats_.capture_frames_since_reset;
  impl_->UpdateMetricsLocked();
  impl_->UpdateConvergenceLocked();
  return domain::Result<AudioProcessorFrame>::success(output);
}

domain::OperationResult WebrtcAudioProcessor::reset(const std::string& reason) {
  if (impl_ == nullptr) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                           "WebRTC 前处理器未初始化");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  impl_->ReleaseLocked();
  ++impl_->stats_.resets;
  impl_->stats_.last_reset_reason = reason;
  const auto result = impl_->InitializeLocked();
  if (!result.ok()) {
    impl_->MarkFailedLocked(reason);
  }
  return result;
}

domain::OperationResult WebrtcAudioProcessor::close() noexcept {
  if (impl_ == nullptr) {
    return domain::OperationResult::success();
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  impl_->ReleaseLocked();
  return domain::OperationResult::success();
}

AudioProcessorState WebrtcAudioProcessor::state() const {
  if (impl_ == nullptr) {
    return AudioProcessorState::kClosed;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->stats_.state;
}

AudioProcessorStats WebrtcAudioProcessor::stats() const {
  if (impl_ == nullptr) {
    return AudioProcessorStats{};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->stats_;
}

}  // namespace nexweave::backend

// kaldi-native-fbank 在线前端实现。只在开启 RKNN 适配器构建时编译。

#include "kaldi_online_fbank.hpp"

#include <cstdint>
#include <limits>
#include <utility>

#include "kaldi-native-fbank/csrc/online-feature.h"

namespace nexweave::backend {

struct KaldiOnlineFbankFrontend::Impl {
  Impl() {
    options.frame_opts.samp_freq = 16000.0F;
    options.frame_opts.frame_shift_ms = 10.0F;
    options.frame_opts.frame_length_ms = 25.0F;
    options.frame_opts.dither = 0.0F;
    options.frame_opts.snip_edges = false;
    options.mel_opts.num_bins = 80;
    options.mel_opts.high_freq = -400.0F;
    fbank = std::make_unique<knf::OnlineFbank>(options);
  }

  knf::FbankOptions options;
  std::unique_ptr<knf::OnlineFbank> fbank;
  std::vector<float> conversion;
  std::size_t discarded_frames = 0;
};

KaldiOnlineFbankFrontend::KaldiOnlineFbankFrontend()
    : impl_(std::make_unique<Impl>()) {}

KaldiOnlineFbankFrontend::~KaldiOnlineFbankFrontend() = default;

std::unique_ptr<KaldiOnlineFbankFrontend> KaldiOnlineFbankFrontend::create() {
  return std::unique_ptr<KaldiOnlineFbankFrontend>(new KaldiOnlineFbankFrontend());
}

void KaldiOnlineFbankFrontend::reset() {
  impl_->fbank = std::make_unique<knf::OnlineFbank>(impl_->options);
  impl_->discarded_frames = 0;
  impl_->conversion.clear();
}

domain::OperationResult KaldiOnlineFbankFrontend::accept(
    const std::int16_t* samples,
    std::size_t count) {
  if (samples == nullptr && count != 0) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "fbank 输入样本为空");
  }
  if (count == 0) {
    return domain::OperationResult::success();
  }
  if (count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "fbank 输入块超过 SDK 上限");
  }
  impl_->conversion.resize(count);
  for (std::size_t index = 0; index < count; ++index) {
    impl_->conversion[index] = static_cast<float>(samples[index]) / 32768.0F;
  }
  impl_->fbank->AcceptWaveform(
      16000.0F, impl_->conversion.data(), static_cast<std::int32_t>(count));
  return domain::OperationResult::success();
}

domain::OperationResult KaldiOnlineFbankFrontend::append_silence(std::size_t count) {
  if (count == 0) {
    return domain::OperationResult::success();
  }
  if (count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "fbank 补零块超过 SDK 上限");
  }
  impl_->conversion.assign(count, 0.0F);
  impl_->fbank->AcceptWaveform(
      16000.0F, impl_->conversion.data(), static_cast<std::int32_t>(count));
  return domain::OperationResult::success();
}

std::size_t KaldiOnlineFbankFrontend::available_feature_frames() const {
  return static_cast<std::size_t>(impl_->fbank->NumFramesReady());
}

const float* KaldiOnlineFbankFrontend::feature_frame(
    std::size_t absolute_index) const {
  if (absolute_index < impl_->discarded_frames) {
    return nullptr;
  }
  return impl_->fbank->GetFrame(static_cast<std::int32_t>(absolute_index));
}

domain::OperationResult KaldiOnlineFbankFrontend::discard_first(
    std::size_t frame_count) {
  if (frame_count == 0) {
    return domain::OperationResult::success();
  }
  const std::size_t total = available_feature_frames();
  if (total < impl_->discarded_frames ||
      frame_count > total - impl_->discarded_frames) {
    return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                            "fbank 丢弃帧数超过保留量");
  }
  if (frame_count >
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "fbank 丢弃帧数超过 SDK 上限");
  }
  impl_->fbank->Pop(static_cast<std::int32_t>(frame_count));
  impl_->discarded_frames += frame_count;
  return domain::OperationResult::success();
}

}  // namespace nexweave::backend

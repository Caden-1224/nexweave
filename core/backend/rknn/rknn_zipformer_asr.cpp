// RKNN Zipformer ASR 适配器实现：把词表、在线 fbank 前端、RKNN 引擎和通用流式核心
// 组装成一个 IAsr。资源所有权在本文件明确：vocabulary 和 stream 由 Impl 拥有，engine
// 与 frontend 移交给 stream；其余错误由 create 返回结构化结果，不在 Session 中展开。
#include "rknn_zipformer_asr.hpp"

#include <limits>
#include <utility>

#include "kaldi_online_fbank.hpp"
#include "rknn_zipformer_engine.hpp"
#include "../zipformer_stream.hpp"

namespace nexweave::backend {

struct RknnZipformerAsr::Impl {
  std::unique_ptr<ZipformerStreamAsr> stream;
  std::string api_version;
  std::string driver_version;
};

RknnZipformerAsr::RknnZipformerAsr(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

RknnZipformerAsr::~RknnZipformerAsr() = default;

domain::Result<std::unique_ptr<RknnZipformerAsr>> RknnZipformerAsr::create(
    const RknnZipformerConfig& config) {
  if (config.encoder_model_path.empty() || config.decoder_model_path.empty() ||
      config.joiner_model_path.empty() || config.vocab_path.empty()) {
    return domain::Result<std::unique_ptr<RknnZipformerAsr>>::failure(
        domain::ErrorCode::kInvalidInput, "RKNN Zipformer 路径配置为空");
  }
  if (config.run_timeout_ms >
      static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
    return domain::Result<std::unique_ptr<RknnZipformerAsr>>::failure(
        domain::ErrorCode::kInvalidInput, "RKNN Zipformer 超时配置溢出");
  }

  auto vocabulary =
      zipformer_detail::load_zipformer_vocabulary(config.vocab_path);
  if (!vocabulary.ok()) {
    return domain::Result<std::unique_ptr<RknnZipformerAsr>>::failure(
        vocabulary.error.code, vocabulary.error.message);
  }

  RknnZipformerEngineOptions engine_options;
  engine_options.encoder_model_path = config.encoder_model_path;
  engine_options.decoder_model_path = config.decoder_model_path;
  engine_options.joiner_model_path = config.joiner_model_path;
  engine_options.blank_token_id = config.blank_token_id;
  engine_options.unk_token_id = config.unk_token_id;
  engine_options.run_timeout_ms = config.run_timeout_ms;
  auto engine = RknnZipformerEngine::create(engine_options);
  if (!engine.ok()) {
    return domain::Result<std::unique_ptr<RknnZipformerAsr>>::failure(
        engine.error.code, engine.error.message);
  }
  RknnZipformerEngine* engine_ptr = engine.value->get();
  const std::string api_version = engine_ptr->sdk_api_version();
  const std::string driver_version = engine_ptr->sdk_driver_version();

  zipformer_detail::ZipformerStreamConfig stream_config;
  stream_config.max_round_samples = config.max_round_samples;
  stream_config.max_hypothesis_tokens = config.max_hypothesis_tokens;
  stream_config.max_tail_pad_chunks = config.max_tail_pad_chunks;
  auto stream = ZipformerStreamAsr::create(
      KaldiOnlineFbankFrontend::create(),
      std::move(*engine.value),
      std::move(*vocabulary.value),
      stream_config);
  if (!stream.ok()) {
    return domain::Result<std::unique_ptr<RknnZipformerAsr>>::failure(
        stream.error.code, stream.error.message);
  }

  auto impl = std::make_unique<Impl>();
  impl->stream = std::move(*stream.value);
  impl->api_version = api_version;
  impl->driver_version = driver_version;
  return domain::Result<std::unique_ptr<RknnZipformerAsr>>::success(
      std::unique_ptr<RknnZipformerAsr>(new RknnZipformerAsr(std::move(impl))));
}

domain::OperationResult RknnZipformerAsr::set_callback(
    capability::TextEventCallback callback) {
  return impl_->stream->set_callback(std::move(callback));
}

domain::OperationResult RknnZipformerAsr::feed(const domain::AudioFrame& frame,
                                               bool is_last) {
  return impl_->stream->feed(frame, is_last);
}

domain::OperationResult RknnZipformerAsr::cancel() noexcept {
  return impl_->stream->cancel();
}

std::string RknnZipformerAsr::sdk_api_version() const {
  return impl_->api_version;
}

std::string RknnZipformerAsr::sdk_driver_version() const {
  return impl_->driver_version;
}

}  // namespace nexweave::backend

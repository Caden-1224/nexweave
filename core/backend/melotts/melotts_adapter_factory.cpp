#include "melotts_adapter_factory.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "melotts_real_engines.hpp"

namespace nexweave::backend {
namespace {

bool file_readable(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  return input.good();
}

domain::Result<std::vector<float>> load_g_vector(const std::string& path) {
  constexpr std::size_t kExpectedFloats = 256U;
  constexpr std::size_t kExpectedBytes = kExpectedFloats * sizeof(float);
  if (!file_readable(path)) {
    return domain::Result<std::vector<float>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS g 文件不可读");
  }
  std::ifstream input(path, std::ios::binary);
  std::vector<float> values(kExpectedFloats, 0.0F);
  input.read(reinterpret_cast<char*>(values.data()),
             static_cast<std::streamsize>(kExpectedBytes));
  if (input.gcount() != static_cast<std::streamsize>(kExpectedBytes) ||
      input.peek() != std::char_traits<char>::eof()) {
    return domain::Result<std::vector<float>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS g 文件必须恰好是 1024 字节");
  }
  return domain::Result<std::vector<float>>::success(std::move(values));
}

}  // namespace

domain::Result<std::unique_ptr<MeloTtsTts>> create_melotts_tts_adapter(
    const MeloTtsConfig& config) {
  if (config.encoder_model_path.empty() || config.decoder_model_path.empty() ||
      config.lexicon_path.empty() || config.tokens_path.empty() ||
      config.g_path.empty()) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 模型/文本资源路径为空");
  }
  if (config.encoder_runtime != MeloTtsRuntime::kOnnxRuntimeCpu ||
      config.decoder_runtime != MeloTtsRuntime::kRknnNpu) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        domain::ErrorCode::kInvalidInput,
        "MeloTTS 只支持 ONNX Runtime CPU 编码器 + RKNN NPU 解码器");
  }

  auto g_values = load_g_vector(config.g_path);
  if (!g_values.ok()) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        g_values.error.code, g_values.error.message);
  }
  auto frontend = MeloTextFrontend::create(config.lexicon_path, config.tokens_path);
  if (!frontend.ok()) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        frontend.error.code, frontend.error.message);
  }

  MeloOrtEncoderConfig encoder_config;
  encoder_config.model_path = config.encoder_model_path;
  encoder_config.intra_op_num_threads = config.intra_op_num_threads;
  auto encoder = MeloOrtEncoder::create(encoder_config);
  if (!encoder.ok()) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        encoder.error.code, encoder.error.message);
  }

  MeloRknnDecoderConfig decoder_config;
  decoder_config.model_path = config.decoder_model_path;
  decoder_config.run_timeout_ms = config.run_timeout_ms;
  auto decoder = MeloRknnDecoder::create(decoder_config);
  if (!decoder.ok()) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        decoder.error.code, decoder.error.message);
  }

  MeloTtsSynthesisOptions options;
  options.g = std::move(*g_values.value);
  options.native_sample_rate_hz = config.native_sample_rate_hz;
  options.speed = config.speed;
  options.noise_scale = config.noise_scale;
  options.noise_scale_w = config.noise_scale_w;
  options.sdp_ratio = config.sdp_ratio;
  options.max_text_bytes = config.max_text_bytes;
  options.max_encoder_phones = config.max_encoder_phones;
  options.max_z_frames_per_chunk = config.max_z_frames_per_chunk;

  return MeloTtsTts::create(std::move(*encoder.value), std::move(*decoder.value),
                            std::move(*frontend.value), std::move(options));
}

}  // namespace nexweave::backend

// Zipformer 流式适配器核心实现。RKNN、kaldi-native-fbank 与文件路径细节都在
// 本文件之外；这里只处理 IAsr 契约、窗口边界、假设状态和回调顺序。

#include "zipformer_stream.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace nexweave::backend {
namespace zipformer_detail {
namespace {

constexpr std::size_t kMaxTailPadChunks = 8U;

bool is_ascii_space(char value) {
  return std::isspace(static_cast<unsigned char>(value)) != 0;
}

std::string trim_ascii_space(std::string value) {
  std::size_t begin = 0;
  while (begin < value.size() && is_ascii_space(value[begin])) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin && is_ascii_space(value[end - 1])) {
    --end;
  }
  return value.substr(begin, end - begin);
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
  if (from.empty()) {
    return;
  }
  std::size_t position = 0;
  while ((position = text.find(from, position)) != std::string::npos) {
    text.replace(position, from.size(), to);
    position += to.size();
  }
}

std::string trim_trailing_carriage_return(std::string line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

}  // namespace

domain::OperationResult validate_zipformer_model_info(const ZipformerModelInfo& info) {
  if (info.feature_dim == 0 || info.chunk_feature_frames < 2 ||
      info.encoder_output_frames == 0 || info.encoder_subsampling_factor == 0 ||
      info.decoder_output_dim == 0 || info.decoder_context_tokens == 0 ||
      info.joiner_output_classes == 0 || info.feature_frame_shift_samples == 0) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "Zipformer 模型几何字段不能为零");
  }
  if (info.encoder_output_frames >
      std::numeric_limits<std::size_t>::max() / info.encoder_subsampling_factor) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "Zipformer 下采样步长溢出");
  }
  const std::size_t advance =
      info.encoder_output_frames * info.encoder_subsampling_factor;
  if (advance == 0 || advance >= info.chunk_feature_frames) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "Zipformer 窗口步长必须小于窗口长度");
  }
  if (info.blank_token_id >= info.joiner_output_classes ||
      info.unk_token_id >= info.joiner_output_classes ||
      info.blank_token_id == info.unk_token_id) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "Zipformer blank/unk token id 非法");
  }
  return domain::OperationResult::success();
}

domain::OperationResult validate_zipformer_stream_config(
    const ZipformerStreamConfig& config) {
  if (config.max_round_samples == 0 || config.max_hypothesis_tokens == 0) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "Zipformer 流式容量不能为零");
  }
  if (config.max_tail_pad_chunks == 0 ||
      config.max_tail_pad_chunks > kMaxTailPadChunks) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "Zipformer 尾窗口补齐次数超出范围");
  }
  return domain::OperationResult::success();
}

domain::Result<std::vector<std::string>> load_zipformer_vocabulary(
    const std::string& path) {
  if (path.empty()) {
    return domain::Result<std::vector<std::string>>::failure(
        domain::ErrorCode::kInvalidInput, "Zipformer 词表路径为空");
  }
  std::ifstream input(path);
  if (!input.is_open()) {
    return domain::Result<std::vector<std::string>>::failure(
        domain::ErrorCode::kInvalidInput, "Zipformer 词表无法打开");
  }

  struct Entry {
    std::string token;
    std::size_t id = 0;
  };
  std::vector<Entry> entries;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    line = trim_trailing_carriage_return(std::move(line));
    if (line.empty()) {
      continue;
    }
    const std::size_t separator = line.find(' ');
    if (separator == std::string::npos || separator == 0) {
      return domain::Result<std::vector<std::string>>::failure(
          domain::ErrorCode::kInvalidInput, "Zipformer 词表行格式非法");
    }
    const std::string token = line.substr(0, separator);
    const std::string id_text = trim_ascii_space(line.substr(separator + 1));
    if (id_text.empty()) {
      return domain::Result<std::vector<std::string>>::failure(
          domain::ErrorCode::kInvalidInput, "Zipformer 词表 id 为空");
    }
    std::size_t consumed = 0;
    std::size_t id = 0;
    try {
      id = static_cast<std::size_t>(std::stoull(id_text, &consumed, 10));
    } catch (const std::exception&) {
      return domain::Result<std::vector<std::string>>::failure(
          domain::ErrorCode::kInvalidInput, "Zipformer 词表 id 不是合法整数");
    }
    if (consumed != id_text.size()) {
      return domain::Result<std::vector<std::string>>::failure(
          domain::ErrorCode::kInvalidInput, "Zipformer 词表 id 含尾随字符");
    }
    entries.push_back(Entry{token, id});
  }
  if (!input.eof()) {
    return domain::Result<std::vector<std::string>>::failure(
        domain::ErrorCode::kBackendFailure, "Zipformer 词表读取失败");
  }
  if (entries.empty()) {
    return domain::Result<std::vector<std::string>>::failure(
        domain::ErrorCode::kInvalidInput, "Zipformer 词表为空");
  }

  std::size_t max_id = 0;
  for (const Entry& entry : entries) {
    max_id = std::max(max_id, entry.id);
  }
  if (max_id == std::numeric_limits<std::size_t>::max()) {
    return domain::Result<std::vector<std::string>>::failure(
        domain::ErrorCode::kInvalidInput, "Zipformer 词表 id 溢出");
  }
  std::vector<std::string> vocabulary(max_id + 1U);
  std::vector<bool> assigned(max_id + 1U, false);
  for (const Entry& entry : entries) {
    if (assigned[entry.id]) {
      return domain::Result<std::vector<std::string>>::failure(
          domain::ErrorCode::kInvalidInput, "Zipformer 词表 id 重复");
    }
    assigned[entry.id] = true;
    vocabulary[entry.id] = entry.token;
  }
  return domain::Result<std::vector<std::string>>::success(std::move(vocabulary));
}

}  // namespace zipformer_detail

struct ZipformerStreamAsr::Impl {
  Impl(std::unique_ptr<zipformer_detail::IFeatureFrontend> feature_frontend,
       std::unique_ptr<zipformer_detail::IZipformerEngine> inference_engine,
       std::vector<std::string> token_table,
       zipformer_detail::ZipformerStreamConfig stream_config)
      : frontend(std::move(feature_frontend)),
        engine(std::move(inference_engine)),
        vocabulary(std::move(token_table)),
        config(stream_config),
        info(engine->info()) {}

  // 清空跨窗口状态并重建前端；失败时对象保持不可继续，调用方必须重新注册。
  domain::OperationResult start_new_round() {
    frontend->reset();
    const auto reset = engine->reset_round();
    if (!reset.ok()) {
      return reset;
    }
    round_open = true;
    round_samples = 0;
    next_feature_frame = 0;
    discarded_feature_frames = 0;
    context.assign(info.decoder_context_tokens,
                   static_cast<std::int64_t>(info.blank_token_id));
    hypothesis.clear();
    full_text.clear();
    last_partial_text.clear();
    encoder_output.clear();
    decoder_output.clear();
    logits.clear();
    decoder_context_initialized = false;
    return domain::OperationResult::success();
  }

  domain::OperationResult emit(capability::TextEvent event) {
    if (cancelled.load()) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
    }
    callback(std::move(event));
    return domain::OperationResult::success();
  }

  std::string render_text() const {
    return zipformer_detail::trim_ascii_space(full_text);
  }

  void append_token_text(std::size_t token_id) {
    std::string piece = vocabulary[token_id];
    zipformer_detail::replace_all(piece, "▁", " ");
    full_text += piece;
  }

  std::size_t advance_feature_frames() const noexcept {
    return info.encoder_output_frames * info.encoder_subsampling_factor;
  }

  domain::OperationResult discard_processed_features() {
    if (next_feature_frame <= discarded_feature_frames) {
      return domain::OperationResult::success();
    }
    const std::size_t count = next_feature_frame - discarded_feature_frames;
    const auto discarded = frontend->discard_first(count);
    if (!discarded.ok()) {
      return discarded;
    }
    discarded_feature_frames = next_feature_frame;
    return domain::OperationResult::success();
  }

  domain::OperationResult run_chunk(std::size_t feature_start) {
    const std::size_t feature_count = info.chunk_feature_frames;
    const std::size_t feature_dim = info.feature_dim;
    if (feature_start > frontend->available_feature_frames() ||
        frontend->available_feature_frames() - feature_start < feature_count) {
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                              "Zipformer 特征窗口数据不足");
    }

    std::vector<float> features(feature_count * feature_dim);
    for (std::size_t index = 0; index < feature_count; ++index) {
      const float* frame = frontend->feature_frame(feature_start + index);
      if (frame == nullptr) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "Zipformer 特征帧为空");
      }
      std::copy_n(frame, feature_dim, features.begin() + index * feature_dim);
    }

    auto encoded = engine->encode_chunk(features.data(), feature_count, encoder_output);
    if (!encoded.ok()) {
      return encoded;
    }
    const std::size_t expected_encoder_values =
        info.encoder_output_frames * info.decoder_output_dim;
    if (encoder_output.size() != expected_encoder_values) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "Zipformer encoder 输出形状不匹配");
    }

    if (!decoder_context_initialized) {
      const auto decoded = engine->decode(context.data(), context.size(), decoder_output);
      if (!decoded.ok()) {
        return decoded;
      }
      if (decoder_output.size() != info.decoder_output_dim) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "Zipformer decoder 输出形状不匹配");
      }
      decoder_context_initialized = true;
    }

    for (std::size_t frame = 0; frame < info.encoder_output_frames; ++frame) {
      if (cancelled.load()) {
        return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
      }
      const float* encoder_frame =
          encoder_output.data() + frame * info.decoder_output_dim;
      const auto joined = engine->join(encoder_frame, decoder_output.data(), logits);
      if (!joined.ok()) {
        return joined;
      }
      if (logits.size() != info.joiner_output_classes) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "Zipformer joiner 输出形状不匹配");
      }
      const std::size_t token_id = argmax(logits, info.joiner_output_classes);
      if (token_id == info.blank_token_id || token_id == info.unk_token_id) {
        continue;
      }
      if (token_id >= vocabulary.size()) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "Zipformer token id 超出词表范围");
      }
      if (hypothesis.size() >= config.max_hypothesis_tokens) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "Zipformer 识别文本超过容量上限");
      }
      hypothesis.push_back(token_id);
      append_token_text(token_id);

      for (std::size_t index = 1; index < context.size(); ++index) {
        context[index - 1] = context[index];
      }
      context.back() = static_cast<std::int64_t>(token_id);
      const auto decoded =
          engine->decode(context.data(), context.size(), decoder_output);
      if (!decoded.ok()) {
        return decoded;
      }
      if (decoder_output.size() != info.decoder_output_dim) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "Zipformer decoder 输出形状不匹配");
      }
    }

    const std::string current_text = render_text();
    if (current_text != last_partial_text) {
      last_partial_text = current_text;
      const auto emitted = emit(capability::TextEvent{
          capability::TextEventKind::kPartial, current_text, {}});
      if (!emitted.ok()) {
        return emitted;
      }
    }
    return domain::OperationResult::success();
  }

  static std::size_t argmax(const std::vector<float>& values, std::size_t count) {
    std::size_t best = 0;
    float best_value = values[0];
    for (std::size_t index = 1; index < count; ++index) {
      if (values[index] > best_value) {
        best_value = values[index];
        best = index;
      }
    }
    return best;
  }

  domain::OperationResult process_available_chunks() {
    while (true) {
      if (cancelled.load()) {
        return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
      }
      const std::size_t available = frontend->available_feature_frames();
      if (available < next_feature_frame) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "Zipformer 特征帧下标倒退");
      }
      if (available - next_feature_frame < info.chunk_feature_frames) {
        break;
      }
      const std::size_t chunk_start = next_feature_frame;
      const auto ran = run_chunk(chunk_start);
      if (!ran.ok()) {
        return ran;
      }
      next_feature_frame = chunk_start + advance_feature_frames();
      const auto discarded = discard_processed_features();
      if (!discarded.ok()) {
        return discarded;
      }
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult ensure_padding_window() {
    const std::size_t required = info.chunk_feature_frames;
    std::size_t attempts = 0;
    while (true) {
      const std::size_t available = frontend->available_feature_frames();
      if (available < next_feature_frame) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "Zipformer 尾帧下标倒退");
      }
      const std::size_t remaining = available - next_feature_frame;
      if (remaining >= required) {
        return domain::OperationResult::success();
      }
      const std::size_t missing = required - remaining;
      if (missing > std::numeric_limits<std::size_t>::max() /
                        info.feature_frame_shift_samples) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "Zipformer 尾帧补零长度溢出");
      }
      const std::size_t sample_count = missing * info.feature_frame_shift_samples;
      const auto appended = frontend->append_silence(sample_count);
      if (!appended.ok()) {
        return appended;
      }
      ++attempts;
      if (attempts > 4U) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "Zipformer 尾帧补零未形成完整窗口");
      }
    }
  }

  domain::OperationResult flush_tail() {
    std::size_t padded_chunks = 0;
    while (padded_chunks < config.max_tail_pad_chunks) {
      if (cancelled.load()) {
        return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
      }
      const std::size_t available = frontend->available_feature_frames();
      if (available < next_feature_frame) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "Zipformer 尾帧下标倒退");
      }
      if (available - next_feature_frame == 0) {
        break;
      }
      const auto padded = ensure_padding_window();
      if (!padded.ok()) {
        return padded;
      }
      const std::size_t chunk_start = next_feature_frame;
      const auto ran = run_chunk(chunk_start);
      if (!ran.ok()) {
        return ran;
      }
      next_feature_frame = chunk_start + advance_feature_frames();
      const auto discarded = discard_processed_features();
      if (!discarded.ok()) {
        return discarded;
      }
      ++padded_chunks;
    }

    if (cancelled.load()) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
    }
    const std::string final_text = render_text();
    auto final_event = emit(capability::TextEvent{
        capability::TextEventKind::kFinal, final_text, {}});
    if (!final_event.ok()) {
      return final_event;
    }
    auto done_event = emit(
        capability::TextEvent{capability::TextEventKind::kDone, "", {}});
    if (!done_event.ok()) {
      return done_event;
    }
    round_open = false;
    return domain::OperationResult::success();
  }

  std::unique_ptr<zipformer_detail::IFeatureFrontend> frontend;
  std::unique_ptr<zipformer_detail::IZipformerEngine> engine;
  std::vector<std::string> vocabulary;
  zipformer_detail::ZipformerStreamConfig config;
  zipformer_detail::ZipformerModelInfo info;
  capability::TextEventCallback callback;

  std::atomic<bool> cancelled{false};
  bool round_open = false;
  std::size_t round_samples = 0;
  std::size_t next_feature_frame = 0;
  std::size_t discarded_feature_frames = 0;
  std::vector<std::int64_t> context;
  std::vector<std::size_t> hypothesis;
  std::string full_text;
  std::string last_partial_text;
  std::vector<float> encoder_output;
  std::vector<float> decoder_output;
  std::vector<float> logits;
  bool decoder_context_initialized = false;
};

ZipformerStreamAsr::ZipformerStreamAsr(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ZipformerStreamAsr::~ZipformerStreamAsr() = default;

domain::Result<std::unique_ptr<ZipformerStreamAsr>> ZipformerStreamAsr::create(
    std::unique_ptr<zipformer_detail::IFeatureFrontend> frontend,
    std::unique_ptr<zipformer_detail::IZipformerEngine> engine,
    std::vector<std::string> vocabulary,
    zipformer_detail::ZipformerStreamConfig config) {
  if (!frontend || !engine) {
    return domain::Result<std::unique_ptr<ZipformerStreamAsr>>::failure(
        domain::ErrorCode::kInvalidInput, "Zipformer 前端或引擎为空");
  }
  const auto info_ok = zipformer_detail::validate_zipformer_model_info(engine->info());
  if (!info_ok.ok()) {
    return domain::Result<std::unique_ptr<ZipformerStreamAsr>>::failure(
        info_ok.error.code, info_ok.error.message);
  }
  const auto config_ok =
      zipformer_detail::validate_zipformer_stream_config(config);
  if (!config_ok.ok()) {
    return domain::Result<std::unique_ptr<ZipformerStreamAsr>>::failure(
        config_ok.error.code, config_ok.error.message);
  }
  if (vocabulary.empty()) {
    return domain::Result<std::unique_ptr<ZipformerStreamAsr>>::failure(
        domain::ErrorCode::kInvalidInput, "Zipformer 词表为空");
  }
  const auto& info = engine->info();
  if (vocabulary.size() < info.joiner_output_classes) {
    return domain::Result<std::unique_ptr<ZipformerStreamAsr>>::failure(
        domain::ErrorCode::kInvalidInput, "Zipformer 词表小于 joiner 输出类别数");
  }
  if (info.blank_token_id >= vocabulary.size() ||
      info.unk_token_id >= vocabulary.size()) {
    return domain::Result<std::unique_ptr<ZipformerStreamAsr>>::failure(
        domain::ErrorCode::kInvalidInput, "Zipformer blank/unk token 超出词表");
  }

  auto impl = std::make_unique<Impl>(std::move(frontend), std::move(engine),
                                     std::move(vocabulary), config);
  return domain::Result<std::unique_ptr<ZipformerStreamAsr>>::success(
      std::unique_ptr<ZipformerStreamAsr>(new ZipformerStreamAsr(std::move(impl))));
}

domain::OperationResult ZipformerStreamAsr::set_callback(
    capability::TextEventCallback callback) {
  if (!callback) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "Zipformer ASR 回调为空");
  }
  const auto started = impl_->start_new_round();
  if (!started.ok()) {
    return started;
  }
  impl_->callback = std::move(callback);
  impl_->cancelled.store(false);
  return domain::OperationResult::success();
}

domain::OperationResult ZipformerStreamAsr::feed(const domain::AudioFrame& frame,
                                                 bool is_last) {
  if (impl_->cancelled.load()) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }
  if (!impl_->callback || !domain::validate_audio_frame(frame).ok()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "Zipformer ASR 帧或回调非法");
  }
  if (!impl_->round_open) {
    const auto started = impl_->start_new_round();
    if (!started.ok()) {
      impl_->cancelled.store(true);
      return started;
    }
    if (impl_->cancelled.load()) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
    }
  }
  if (impl_->round_samples > impl_->config.max_round_samples ||
      frame.samples.size() >
          impl_->config.max_round_samples - impl_->round_samples) {
    return domain::OperationResult::failure(
        domain::ErrorCode::kBackendFailure, "Zipformer 本轮音频超过容量上限");
  }

  try {
    const auto accepted =
        impl_->frontend->accept(frame.samples.data(), frame.samples.size());
    if (!accepted.ok()) {
      impl_->cancelled.store(true);
      return accepted;
    }
    impl_->round_samples += frame.samples.size();

    const auto processed = impl_->process_available_chunks();
    if (!processed.ok()) {
      if (!processed.error.cancelled()) {
        impl_->cancelled.store(true);
      }
      return processed;
    }
    if (is_last) {
      const auto flushed = impl_->flush_tail();
      if (!flushed.ok()) {
        if (!flushed.error.cancelled()) {
          impl_->cancelled.store(true);
        }
        return flushed;
      }
    }
  } catch (...) {
    impl_->cancelled.store(true);
    impl_->round_open = false;
    throw;
  }
  return domain::OperationResult::success();
}

domain::OperationResult ZipformerStreamAsr::cancel() noexcept {
  impl_->cancelled.store(true);
  return domain::OperationResult::success();
}

}  // namespace nexweave::backend

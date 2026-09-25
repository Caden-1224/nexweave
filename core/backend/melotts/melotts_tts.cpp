#include "melotts_tts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "melotts_pcm_converter.hpp"

namespace nexweave::backend {
namespace {

struct DecodeSlice {
  std::size_t unit_begin = 0;
  std::size_t unit_end = 0;
  std::size_t frame_begin = 0;
  std::size_t frame_end = 0;
};

// 计算左闭右开区间 [begin,end) 的帧数之和。所有调用点都先做边界检查，因此这里
// 只负责累加，不处理越界；返回值用 size_t 表达个数量级很小的 z_p 帧数。
std::size_t range_sum(const std::vector<std::size_t>& values,
                      std::size_t begin,
                      std::size_t end) {
  std::size_t total = 0;
  for (std::size_t index = begin; index < end; ++index) {
    total += values[index];
  }
  return total;
}

// 把单词级帧数切成不超过 frames_per_call 的解码切片，并尽量在相邻切片之间保留
// 最多两个 unit 的上下文。返回的切片在 frame 轴上连续覆盖 [0,total)，重叠部分
// 由调用方在拼接时丢弃后一切片的头部，保证每个 z_p 帧只输出一次音频。
domain::Result<std::vector<DecodeSlice>> build_decode_slices(
    const std::vector<std::size_t>& unit_frames,
    std::size_t frames_per_call) {
  if (frames_per_call == 0U) {
    return domain::Result<std::vector<DecodeSlice>>::failure(
        domain::ErrorCode::kBackendFailure, "MeloTTS 解码器单次帧数预算为 0");
  }

  std::vector<std::size_t> bounded;
  for (const std::size_t frames : unit_frames) {
    std::size_t remaining = frames;
    while (remaining > frames_per_call) {
      bounded.push_back(frames_per_call);
      remaining -= frames_per_call;
    }
    if (remaining > 0U) {
      bounded.push_back(remaining);
    }
  }
  if (bounded.empty()) {
    return domain::Result<std::vector<DecodeSlice>>::failure(
        domain::ErrorCode::kBackendFailure, "MeloTTS 编码器没有产生可解码帧");
  }

  std::vector<DecodeSlice> slices;
  std::size_t unit_begin = 0;
  std::size_t unit_end = 0;
  std::size_t frame_end = 0;
  while (unit_end < bounded.size()) {
    std::size_t slice_unit_begin = unit_end;
    std::size_t slice_frame_begin = frame_end;
    std::size_t slice_frames = 0;

    // 与文本前端一致的词边界上下文策略：只有前一个切片已经跨过两个以上 unit，
    // 且“回退两个 unit + 当前 unit”仍能装下时，才把这两个 unit 作为重叠上下文。
    if (unit_end - unit_begin > 2U &&
        range_sum(bounded, unit_end - 2U, unit_end + 1U) <= frames_per_call) {
      slice_frames = range_sum(bounded, unit_end - 2U, unit_end);
      slice_frame_begin = frame_end - slice_frames;
      slice_unit_begin = unit_end - 2U;
      unit_begin = unit_end - 2U;
    } else {
      slice_frames = 0;
      slice_frame_begin = frame_end;
      slice_unit_begin = unit_end;
      unit_begin = unit_end;
    }

    while (unit_end < bounded.size() &&
           slice_frames + bounded[unit_end] <= frames_per_call) {
      slice_frames += bounded[unit_end];
      ++unit_end;
    }
    if (slice_frames == 0U) {
      return domain::Result<std::vector<DecodeSlice>>::failure(
          domain::ErrorCode::kBackendFailure, "MeloTTS 解码切片预算无法容纳音素帧");
    }
    slices.push_back(DecodeSlice{slice_unit_begin, unit_end, slice_frame_begin,
                                 slice_frame_begin + slice_frames});
    frame_end = slice_frame_begin + slice_frames;
  }
  return domain::Result<std::vector<DecodeSlice>>::success(std::move(slices));
}

domain::Result<std::vector<std::size_t>> make_word_frame_counts(
    const MeloPhoneChunk& chunk,
    const std::vector<std::int32_t>& phone_lengths) {
  if (phone_lengths.size() != chunk.phones.size()) {
    return domain::Result<std::vector<std::size_t>>::failure(
        domain::ErrorCode::kBackendFailure, "MeloTTS phone_lengths 与输入长度不一致");
  }
  std::vector<std::size_t> counts;
  counts.reserve(chunk.word_phone_counts.size());
  std::size_t phone_index = 0;
  for (const std::size_t phone_count : chunk.word_phone_counts) {
    if (phone_count == 0U) {
      counts.push_back(0U);
      continue;
    }
    if (phone_index + phone_count > phone_lengths.size()) {
      return domain::Result<std::vector<std::size_t>>::failure(
          domain::ErrorCode::kBackendFailure, "MeloTTS word_phone_counts 越界");
    }
    std::size_t frames = 0;
    for (std::size_t index = 0; index < phone_count; ++index) {
      const std::int32_t length = phone_lengths[phone_index + index];
      if (length < 0) {
        return domain::Result<std::vector<std::size_t>>::failure(
            domain::ErrorCode::kBackendFailure, "MeloTTS 预测出负的发音时长");
      }
      frames += static_cast<std::size_t>(length);
    }
    counts.push_back(frames);
    phone_index += phone_count;
  }
  if (phone_index != phone_lengths.size()) {
    return domain::Result<std::vector<std::size_t>>::failure(
        domain::ErrorCode::kBackendFailure, "MeloTTS word_phone_counts 未覆盖全部音素");
  }
  return domain::Result<std::vector<std::size_t>>::success(std::move(counts));
}

bool finite_non_negative(float value) {
  return std::isfinite(value) && value >= 0.0F;
}

}  // namespace

MeloTtsTts::MeloTtsTts(std::unique_ptr<IMeloEncoder> encoder,
                       std::unique_ptr<IMeloDecoder> decoder,
                       MeloTextFrontend frontend,
                       MeloTtsSynthesisOptions options)
    : encoder_(std::move(encoder)),
      decoder_(std::move(decoder)),
      frontend_(std::move(frontend)),
      options_(std::move(options)) {}

domain::Result<std::unique_ptr<MeloTtsTts>> MeloTtsTts::create(
    std::unique_ptr<IMeloEncoder> encoder,
    std::unique_ptr<IMeloDecoder> decoder,
    MeloTextFrontend frontend,
    MeloTtsSynthesisOptions options) {
  if (!encoder || !decoder) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 编码器或解码器为空");
  }
  if (frontend.lexicon_entry_count() == 0U) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 文本前端未加载");
  }
  if (options.g.empty()) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS g 向量为空");
  }
  if (options.native_sample_rate_hz == 0U) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 原生采样率为 0");
  }
  if (!std::isfinite(options.speed) || options.speed <= 0.0F ||
      !finite_non_negative(options.noise_scale) ||
      !finite_non_negative(options.noise_scale_w) ||
      !finite_non_negative(options.sdp_ratio)) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 推理参数非法");
  }
  if (options.max_text_bytes == 0U || options.max_encoder_phones < 3U ||
      options.max_z_frames_per_chunk == 0U) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 容量参数非法");
  }
  const MeloDecoderInfo& decoder_info = decoder->info();
  if (decoder_info.channels_per_frame == 0U || decoder_info.frames_per_call == 0U ||
      decoder_info.samples_per_frame == 0U) {
    return domain::Result<std::unique_ptr<MeloTtsTts>>::failure(
        domain::ErrorCode::kBackendFailure, "MeloTTS 解码器形状信息非法");
  }
  return domain::Result<std::unique_ptr<MeloTtsTts>>::success(
      std::unique_ptr<MeloTtsTts>(new MeloTtsTts(std::move(encoder), std::move(decoder),
                                                 std::move(frontend), std::move(options))));
}

domain::OperationResult MeloTtsTts::set_callback(
    capability::AudioEventCallback callback) {
  if (!callback) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "MeloTTS 回调为空");
  }
  callback_ = std::move(callback);
  cancelled_.store(false);
  statistics_ = {};
  return domain::OperationResult::success();
}

domain::OperationResult MeloTtsTts::synthesize(const std::string& text) {
  if (cancelled_.load()) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled,
                                            "MeloTTS 当前轮次已取消");
  }
  if (!callback_) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "MeloTTS 回调未注册");
  }
  if (text.empty()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "MeloTTS 输入文本为空");
  }
  if (text.size() > options_.max_text_bytes) {
    return domain::OperationResult::failure(
        domain::ErrorCode::kInvalidInput,
        "MeloTTS 文本超过 max_text_bytes，拒绝静默截断");
  }

  statistics_ = {};
  statistics_.input_text_bytes = text.size();
  try {
    return run_synthesis(text);
  } catch (...) {
    // 回调抛出的异常、容器分配异常等一律原样传播；同时用取消位封锁本轮，
    // 避免调用方重试后把已经交付的部分音频再次拼接到新轮次。
    cancelled_.store(true);
    throw;
  }
}

bool MeloTtsTts::emit_frames(std::vector<std::int16_t>& frame_buffer,
                             bool flush_tail) {
  while (frame_buffer.size() >= domain::kAudioFrameSamples) {
    if (cancelled_.load()) {
      return false;
    }
    domain::AudioFrame frame;
    frame.samples.assign(frame_buffer.begin(),
                         frame_buffer.begin() +
                             static_cast<std::ptrdiff_t>(domain::kAudioFrameSamples));
    callback_(frame);
    ++statistics_.delivered_frames;
    frame_buffer.erase(frame_buffer.begin(),
                       frame_buffer.begin() +
                           static_cast<std::ptrdiff_t>(domain::kAudioFrameSamples));
  }

  if (flush_tail && !frame_buffer.empty()) {
    if (cancelled_.load()) {
      return false;
    }
    const std::size_t valid_samples = frame_buffer.size();
    domain::AudioFrame frame;
    frame.samples = std::move(frame_buffer);
    frame.samples.resize(domain::kAudioFrameSamples, 0);
    callback_(frame);
    ++statistics_.delivered_frames;
    frame_buffer.clear();
    statistics_.last_frame_valid_samples = valid_samples;
    statistics_.tail_kind = MeloTtsStatistics::TailKind::kPaddedTail;
  }

  if (flush_tail && statistics_.delivered_frames > 0U && frame_buffer.empty() &&
      statistics_.tail_kind == MeloTtsStatistics::TailKind::kNoOutput) {
    if (statistics_.resampled_samples % domain::kAudioFrameSamples == 0U) {
      statistics_.last_frame_valid_samples = domain::kAudioFrameSamples;
      statistics_.tail_kind = MeloTtsStatistics::TailKind::kFullFrame;
    }
  }
  return true;
}

domain::OperationResult MeloTtsTts::run_synthesis(const std::string& text) {
  auto chunks = frontend_.convert(text, options_.max_encoder_phones);
  if (!chunks.ok()) {
    return domain::OperationResult::failure(chunks.error.code, chunks.error.message);
  }
  if (chunks.value->empty()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "MeloTTS 文本前端没有产生片段");
  }
  statistics_.text_chunks = chunks.value->size();

  const MeloDecoderInfo& decoder_info = decoder_->info();
  MeloTtsPcmConverter converter(options_.native_sample_rate_hz,
                                domain::kAudioSampleRateHz);
  if (!converter.valid()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "MeloTTS 重采样采样率非法");
  }

  std::vector<std::int16_t> frame_buffer;
  frame_buffer.reserve(domain::kAudioFrameSamples);

  for (const MeloPhoneChunk& chunk : *chunks.value) {
    if (cancelled_.load()) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled,
                                              "MeloTTS 合成在文本片段间取消");
    }
    if (chunk.phones.size() != chunk.tones.size() ||
        chunk.phones.size() != chunk.languages.size()) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "MeloTTS 文本片段张量长度不一致");
    }

    MeloEncoderRequest request;
    request.phones = chunk.phones.data();
    request.phone_count = chunk.phones.size();
    request.tones = chunk.tones.data();
    request.tone_count = chunk.tones.size();
    request.languages = chunk.languages.data();
    request.language_count = chunk.languages.size();
    request.g = options_.g.data();
    request.g_count = options_.g.size();
    request.noise_scale = options_.noise_scale;
    request.noise_scale_w = options_.noise_scale_w;
    request.length_scale = 1.0F / options_.speed;
    request.sdp_ratio = options_.sdp_ratio;

    MeloEncoderResponse response;
    const domain::OperationResult encoded = encoder_->run(request, response);
    if (!encoded.ok()) {
      return encoded;
    }
    ++statistics_.encoder_runs;
    if (response.channels != decoder_info.channels_per_frame ||
        response.channels == 0U || response.frames == 0U ||
        response.z_p.size() != response.channels * response.frames ||
        response.phone_lengths.size() != chunk.phones.size()) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "MeloTTS 编码器输出形状与解码器不匹配");
    }
    if (response.frames > options_.max_z_frames_per_chunk) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "MeloTTS 编码器输出超过 z_p 帧预算");
    }

    auto word_frame_counts = make_word_frame_counts(chunk, response.phone_lengths);
    if (!word_frame_counts.ok()) {
      return domain::OperationResult::failure(word_frame_counts.error.code,
                                              word_frame_counts.error.message);
    }
    const std::size_t total_frames = range_sum(*word_frame_counts.value, 0U,
                                               word_frame_counts.value->size());
    if (total_frames != response.frames) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "MeloTTS 发音时长之和与 z_p 帧数不一致");
    }
    const std::size_t expected_native_samples =
        total_frames * decoder_info.samples_per_frame;
    if (response.audio_len_samples < 0 ||
        static_cast<std::size_t>(response.audio_len_samples) !=
            expected_native_samples) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "MeloTTS 原生采样数与时长相位不一致");
    }

    auto slices = build_decode_slices(*word_frame_counts.value,
                                      decoder_info.frames_per_call);
    if (!slices.ok()) {
      return domain::OperationResult::failure(slices.error.code,
                                              slices.error.message);
    }

    std::size_t previous_frame_end = 0U;
    std::size_t slice_index = 0U;
    for (const DecodeSlice& slice : *slices.value) {
      if (cancelled_.load()) {
        return domain::OperationResult::failure(domain::ErrorCode::kCancelled,
                                                "MeloTTS 合成在解码切片间取消");
      }
      if (slice.frame_begin > previous_frame_end || slice.frame_end < previous_frame_end) {
        // 允许 frame_begin 落在 previous_frame_end 之前（上下文重叠）；禁止出现
        // 空洞或向后覆盖，否则拼接会丢帧或重复覆盖已交付的 z_p 帧。
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "MeloTTS 解码切片覆盖关系非法");
      }
      const std::size_t frame_count = slice.frame_end - slice.frame_begin;
      if (frame_count == 0U || frame_count > decoder_info.frames_per_call) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "MeloTTS 解码切片帧数非法");
      }

      std::vector<float> native_audio;
      const domain::OperationResult decoded =
          decoder_->decode(response.z_p.data(), response.frames, slice.frame_begin,
                           frame_count, native_audio);
      if (!decoded.ok()) {
        return decoded;
      }
      ++statistics_.decoder_runs;
      if (native_audio.size() != frame_count * decoder_info.samples_per_frame) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "MeloTTS 解码器返回样本数不符合形状");
      }
      if (cancelled_.load()) {
        return domain::OperationResult::failure(domain::ErrorCode::kCancelled,
                                                "MeloTTS 合成在解码后取消");
      }

      std::size_t drop_samples = 0U;
      if (slice_index > 0U) {
        if (previous_frame_end < slice.frame_begin) {
          return domain::OperationResult::failure(
              domain::ErrorCode::kBackendFailure, "MeloTTS 解码切片没有连续覆盖 z_p");
        }
        drop_samples = (previous_frame_end - slice.frame_begin) *
                       decoder_info.samples_per_frame;
      }
      if (drop_samples > native_audio.size()) {
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "MeloTTS 重叠样本数超过切片长度");
      }
      const std::size_t keep_samples = native_audio.size() - drop_samples;
      if (keep_samples > 0U) {
        statistics_.native_samples += keep_samples;
        std::vector<std::int16_t> converted;
        converter.push(native_audio.data() + drop_samples, keep_samples, converted);
        for (const std::int16_t sample : converted) {
          frame_buffer.push_back(sample);
        }
        if (!emit_frames(frame_buffer, false)) {
          return domain::OperationResult::failure(domain::ErrorCode::kCancelled,
                                                  "MeloTTS 回调取消");
        }
      }
      previous_frame_end = std::max(previous_frame_end, slice.frame_end);
      ++slice_index;
    }
    if (previous_frame_end != response.frames) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "MeloTTS 解码切片没有覆盖全部 z_p 帧");
    }
  }

  if (cancelled_.load()) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled,
                                            "MeloTTS 合成完成前取消");
  }
  std::vector<std::int16_t> tail_samples;
  converter.flush(tail_samples);
  statistics_.resampled_samples = converter.total_output_samples();
  for (const std::int16_t sample : tail_samples) {
    frame_buffer.push_back(sample);
  }
  if (!emit_frames(frame_buffer, true)) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled,
                                            "MeloTTS 尾帧回调取消");
  }
  if (cancelled_.load()) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled,
                                            "MeloTTS 尾帧后取消");
  }
  if (statistics_.delivered_frames == 0U || statistics_.resampled_samples == 0U) {
    return domain::OperationResult::failure(
        domain::ErrorCode::kBackendFailure, "MeloTTS 没有产生可交付 PCM");
  }
  return domain::OperationResult::success();
}

domain::OperationResult MeloTtsTts::cancel() noexcept {
  cancelled_.store(true);
  return domain::OperationResult::success();
}

}  // namespace nexweave::backend

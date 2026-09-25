#include "melotts_pcm_converter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nexweave::backend {
namespace {

// S16 满量程转换：[-1,1] 映射到 [-32768,32767]，越界样本先裁剪。
// 这里不使用“修改帧头”式的伪重采样；每个输出样本都由线性插值计算后量化。
std::int16_t float_to_s16(float sample) noexcept {
  if (!std::isfinite(sample)) {
    return 0;
  }
  const float scaled = std::max(-32768.0F, std::min(32767.0F, sample * 32768.0F));
  if (scaled >= 32767.0F) {
    return std::numeric_limits<std::int16_t>::max();
  }
  if (scaled <= -32768.0F) {
    return std::numeric_limits<std::int16_t>::min();
  }
  return static_cast<std::int16_t>(std::lrint(scaled));
}

}  // namespace

MeloTtsPcmConverter::MeloTtsPcmConverter(std::uint32_t input_rate_hz,
                                         std::uint32_t output_rate_hz)
    : input_rate_hz_(input_rate_hz),
      output_rate_hz_(output_rate_hz),
      ratio_(output_rate_hz == 0 ? 0.0
                                 : static_cast<double>(input_rate_hz) /
                                       static_cast<double>(output_rate_hz)) {}

bool MeloTtsPcmConverter::can_emit_next() const noexcept {
  if (!valid() || next_output_index_ == std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  const long double source =
      static_cast<long double>(next_output_index_) * static_cast<long double>(ratio_);
  const std::size_t index = static_cast<std::size_t>(std::floor(source));
  if (index >= total_input_samples_) {
    return false;
  }
  if (flushed_) {
    // flush 的目标样本数已经由输入时长计算出来；最后一个输出区间可能落在
    // 最后两个输入样本之间。此时 sample_at 对越界的第二个点使用末尾样本，
    // 即零阶保持，避免末尾时间区间被截掉。
    return true;
  }
  const long double fraction = source - static_cast<long double>(index);
  if (fraction == 0.0L) {
    return true;
  }
  return index + 1 < total_input_samples_;
}

float MeloTtsPcmConverter::sample_at(std::size_t global_index) const noexcept {
  if (global_index < input_base_) {
    return 0.0F;
  }
  const std::size_t local_index = global_index - input_base_;
  if (local_index >= input_.size()) {
    return input_.empty() ? 0.0F : input_.back();
  }
  return input_[local_index];
}

void MeloTtsPcmConverter::compact_before(std::size_t global_index) {
  if (global_index <= input_base_) {
    return;
  }
  const std::size_t drop_count = global_index - input_base_;
  if (drop_count > input_.size()) {
    input_.clear();
    input_base_ = total_input_samples_;
    return;
  }
  input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(drop_count));
  input_base_ = global_index;
}

void MeloTtsPcmConverter::drain(std::size_t target,
                                std::vector<std::int16_t>& output) {
  if (!valid()) {
    return;
  }
  while (next_output_index_ < target && can_emit_next()) {
    const long double source =
        static_cast<long double>(next_output_index_) * static_cast<long double>(ratio_);
    const std::size_t index = static_cast<std::size_t>(std::floor(source));
    const float fraction = static_cast<float>(source - static_cast<long double>(index));
    const float first = sample_at(index);
    const float second = sample_at(index + 1);
    output.push_back(float_to_s16(first + (second - first) * fraction));
    ++next_output_index_;
    compact_before(index);
  }
}

void MeloTtsPcmConverter::push(const float* samples,
                               std::size_t count,
                               std::vector<std::int16_t>& output) {
  if (!valid() || samples == nullptr || count == 0 || flushed_) {
    return;
  }
  input_.insert(input_.end(), samples, samples + count);
  total_input_samples_ += count;
  // 流式阶段不预知最终目标数量，只输出已经能完整插值的样本；flush 再补尾。
  drain(std::numeric_limits<std::size_t>::max(), output);
}

void MeloTtsPcmConverter::flush(std::vector<std::int16_t>& output) {
  if (!valid() || flushed_) {
    return;
  }
  flushed_ = true;
  const long double ratio =
      static_cast<long double>(input_rate_hz_) / static_cast<long double>(output_rate_hz_);
  if (total_input_samples_ == 0) {
    return;
  }
  const long double exact_output =
      static_cast<long double>(total_input_samples_) / ratio;
  const std::size_t target = static_cast<std::size_t>(std::floor(exact_output));
  drain(target, output);
  input_.clear();
  input_base_ = total_input_samples_;
}

}  // namespace nexweave::backend

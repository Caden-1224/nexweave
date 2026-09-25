// MeloTTS 原生 float PCM 到 NexWeave 固定 16 kHz S16_LE 帧的流式转换器。
//
// 职责：在合成过程中跨解码器切片保存线性重采样状态，输出连续的 16 kHz int16
// 样本；不负责切 320 样本帧、不负责回调，也不负责尾部补零。调用方把 push/flush
// 得到的样本追加到帧缓冲区，只有帧数达到 320 才构造 AudioFrame。
//
// 输入前提：constructor 的输入/输出采样率都必须 >0；输入样本是单声道 float，
// 取值范围可超过 [-1,1]，实现按 S16 满量程裁剪并记录非线性事实。push 可以任意
// 次数、任意长度地调用，但调用方必须按时间顺序拼接；flush 只能调用一次，且之后
// 不能再 push。
//
// 输出后置：push 输出满足当前输入时长的所有可确定输出样本；flush 输出剩余样本，
// 数量为 floor(total_input_samples * output_rate / input_rate)。重采样使用线性
// 插值，跨 push 的状态只包含尚未消费的输入样本与下一个输出样本序号，因此分块
// 送入与一次性送入得到逐样本一致的结果。
//
// 并发：对象无内部同步，同一实例由单线程串行使用；不创建线程、文件或设备。
// 资源：只拥有标准库 vector；分配异常按标准库规则传播，抛出时对象状态保持到
// 上一次成功返回后的可继续状态。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nexweave::backend {

class MeloTtsPcmConverter {
 public:
  // input_rate_hz/output_rate_hz 必须为正；否则 valid() 为 false，push/flush
  // 不产生任何输出。构造不分配设备资源，只初始化整数状态。
  MeloTtsPcmConverter(std::uint32_t input_rate_hz, std::uint32_t output_rate_hz);

  // 采样率配置是否有效（均 >0）。false 时所有操作都是空操作，调用方应拒绝使用。
  bool valid() const noexcept { return input_rate_hz_ > 0 && output_rate_hz_ > 0; }

  // 追加 count 个单声道 float 样本，并把当前可确定的输出样本追加到 output。
  // samples 为空时是空操作。output 由调用方拥有，已有内容不会被清空。
  void push(const float* samples, std::size_t count, std::vector<std::int16_t>& output);

  // 标记输入结束，计算并追加剩余输出样本。只允许调用一次；重复调用是空操作。
  // 返回后对象不再接受 push，调用方应丢弃该实例。
  void flush(std::vector<std::int16_t>& output);

  // 已 push 的输入样本总数和当前已输出样本数；用于测试和时长审计。
  std::size_t total_input_samples() const noexcept { return total_input_samples_; }
  std::size_t total_output_samples() const noexcept { return next_output_index_; }

 private:
  // 在已有输入范围内尽可能输出；target 为 SIZE_MAX 表示尚不知道最终输出数量。
  void drain(std::size_t target, std::vector<std::int16_t>& output);
  // 返回当前可确定输出的源位置；false 表示还需要后续输入。
  bool can_emit_next() const noexcept;
  float sample_at(std::size_t global_index) const noexcept;
  void compact_before(std::size_t global_index);

  std::uint32_t input_rate_hz_ = 0;
  std::uint32_t output_rate_hz_ = 0;
  double ratio_ = 0.0;  // input_rate_hz_ / output_rate_hz_。
  std::vector<float> input_;
  std::size_t input_base_ = 0;  // input_[0] 在整个输入流中的全局下标。
  std::size_t total_input_samples_ = 0;
  std::size_t next_output_index_ = 0;
  bool flushed_ = false;
};

}  // namespace nexweave::backend

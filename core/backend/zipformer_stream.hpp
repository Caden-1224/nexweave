// Zipformer 流式 ASR 的适配器核心。
//
// 本文件把“逐 20 ms 音频帧驱动、同步回调”的 IAsr 契约，与 Zipformer 三模型
// （encoder/decoder/joiner）的固定特征窗口和增量 greedy 搜索粘合起来。核心只依赖
// 两个窄接口：IFeatureFrontend 负责把 16 kHz/S16_LE 单声道样本变成 80 维 fbank
// 特征，IZipformerEngine 负责执行 RKNN / 模拟推理。RKNN、kaldi-native-fbank 和具体
// 模型句柄都不会进入本文件的公共类型，也不会进入 Session。
//
// 一轮的生命周期：
//   create() 成功或 set_callback() 成功：清空引擎缓存、特征前端和假设，进入空闲。
//   feed(frame, false)*：追加音频，按固定窗口触发 encode/join/decode，文本变化时发 partial。
//   feed(frame, true)：先处理已满窗口，再对尾帧补显式零采样形成最后一次窗口，发 final 与 done，
//                     最后回到空闲；下一次 feed 会自动开始新轮。
//   cancel()：置取消标志；线性化点后不再开始新回调，之后 feed 返回 kCancelled，直到成功注册。
//
// 线程约定：set_callback、feed、reset 由调用方串行调用；cancel() 只写原子标志，可从控制线程
// 调用，不会等待正在执行的 RKNN 推理。已进入的回调不会被取消撤回；取消只保证不再开始新回调。
// 回调异常：适配器把当前轮标记为取消并向上传播异常，避免调用方重试同一轮时重复 partial/final。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../capability/backend.hpp"

namespace nexweave::backend::zipformer_detail {

// 模型几何与词典约定。真实实现从 RKNN 张量属性填充，测试实现用同一结构描述脚本化模型；
// 适配器核心只按这些数值做窗口、边界和索引校验，不猜测具体模型文件名或转换工具行为。
struct ZipformerModelInfo {
  // 每个特征帧的维度；Zipformer 双语模型当前为 80 维 fbank。
  std::size_t feature_dim = 80;
  // encoder 一次计算窗口的特征帧数；该值必须与模型输入张量形状一致。
  std::size_t chunk_feature_frames = 103;
  // encoder 一次计算输出的编码帧数。
  std::size_t encoder_output_frames = 24;
  // 编码器时间下采样倍数；advance = encoder_output_frames * encoder_subsampling_factor。
  std::size_t encoder_subsampling_factor = 4;
  // decoder 输出维度。
  std::size_t decoder_output_dim = 512;
  // decoder 输入携带的历史 token 数，当前为 2。
  std::size_t decoder_context_tokens = 2;
  // joiner 输出类别数；即 argmax 的候选范围。
  std::size_t joiner_output_classes = 6254;
  // 一个特征帧对应的原始采样数，10 ms @ 16 kHz 时为 160。
  std::size_t feature_frame_shift_samples = 160;
  // blank 与 unk 的稳定 token id；必须小于 joiner_output_classes。
  std::size_t blank_token_id = 0;
  std::size_t unk_token_id = 2;
};

// 特征前端：拥有在线 fbank 窗口和少量样本余数。reset() 后通常立即处于新轮起点。
// 所有方法由 feed 调用线程同步调用；feature_frame 返回的内部指针只在下一次 accept/
// append_silence/discard_first/reset 之前有效。available_feature_frames 返回自轮次开始
// 累计产生的特征帧数，即使旧帧被 discard_first 回收，索引语义仍保持不变。
class IFeatureFrontend {
 public:
  virtual ~IFeatureFrontend() = default;

  virtual void reset() = 0;

  // 追加一段 16 kHz 单声道 S16_LE 样本；count 为 0 时合法且无副作用。
  virtual domain::OperationResult accept(const std::int16_t* samples, std::size_t count) = 0;

  // 追加 count 个零值样本，用于结束刷新；count 为 0 时合法且无副作用。
  virtual domain::OperationResult append_silence(std::size_t count) = 0;

  virtual std::size_t available_feature_frames() const = 0;

  // absolute_index 是自 reset 起累计的特征帧下标；调用者必须保证该帧尚未被 discard。
  virtual const float* feature_frame(std::size_t absolute_index) const = 0;

  // 丢弃当前最旧的前 frame_count 个特征帧；frame_count 不得超过当前可丢弃帧数。
  virtual domain::OperationResult discard_first(std::size_t frame_count) = 0;
};

// 模型引擎：持有 encoder/decoder/joiner 的缓存与外部 SDK 资源，不拥有回调或会话状态。
// 同一实例由适配器核心串行调用；encode_chunk 成功返回后，encoder 缓存即进入下一窗口的状态。
class IZipformerEngine {
 public:
  virtual ~IZipformerEngine() = default;

  virtual const ZipformerModelInfo& info() const noexcept = 0;

  // 清空所有跨窗口状态，使下一次 encode_chunk 从新轮边界开始；幂等。
  virtual domain::OperationResult reset_round() = 0;

  // frame_count 必须等于 info().chunk_feature_frames；features 按帧优先连续存储，
  // 每帧 info().feature_dim 个 float。成功时 encoder_output 为
  // encoder_output_frames * decoder_output_dim 个连续 float。
  virtual domain::OperationResult encode_chunk(const float* features,
                                               std::size_t frame_count,
                                               std::vector<float>& encoder_output) = 0;

  // context 为最近 decoder_context_tokens 个 token；成功时 decoder_output 为
  // decoder_output_dim 个 float。调用方拥有输入和输出存储。
  virtual domain::OperationResult decode(const std::int64_t* context,
                                         std::size_t context_size,
                                         std::vector<float>& decoder_output) = 0;

  // 计算单帧编码输出与 decoder 输出的 joiner 分值；成功时 logits 为
  // joiner_output_classes 个 float，调用方负责 argmax。
  virtual domain::OperationResult join(const float* encoder_output,
                                       const float* decoder_output,
                                       std::vector<float>& logits) = 0;
};

// 适配器核心可调参数。模型窗口和词典 id 来自 ZipformerModelInfo，不能在本结构里改写；
// 本结构只放内存与尾帧策略边界，避免把阈值硬编码进循环。
struct ZipformerStreamConfig {
  // 单轮允许提交的原始样本总数，默认 10 分钟；超限返回 kBackendFailure 且不消费本次帧。
  std::size_t max_round_samples = 16000U * 600U;
  // 单轮假设允许的 token 数，默认 20000；超限返回 kBackendFailure，防止长输入无界占用。
  std::size_t max_hypothesis_tokens = 20000U;
  // 结束刷新最多补几轮零样本窗口。默认 1，与上游流式示例在输入结束后的滑窗补齐一致；
  // 增大该值会继续用零音频推进缓存，只用于实验，不得当作真实语音继续识别。
  std::size_t max_tail_pad_chunks = 1U;
};

// 校验模型几何。所有字段在首次 feed 前显式验证；有任何不合法都返回 kInvalidInput。
domain::OperationResult validate_zipformer_model_info(const ZipformerModelInfo& info);

// 校验运行边界。校验失败返回 kInvalidInput，适配器不会带着非法容量进入工作。
domain::OperationResult validate_zipformer_stream_config(const ZipformerStreamConfig& config);

// 读取 Zipformer vocab.txt，返回按 id 索引的 token 表。id 必须非负、唯一且行格式为
// `<token> <id>`；失败返回 kInvalidInput。函数只读文件，不创建线程或设备句柄。
domain::Result<std::vector<std::string>> load_zipformer_vocabulary(const std::string& path);

}  // namespace nexweave::backend::zipformer_detail

namespace nexweave::backend {

// Zipformer 流式 ASR 适配器核心。构造成功后已经拥有 frontend 和 engine；析构按成员顺序
// 释放（engine 先释放模型句柄，frontend 再释放特征窗口）。对象本身不拥有线程。
class ZipformerStreamAsr final : public capability::IAsr {
 public:
  // 失败时返回结构化错误且不创建对象：空 frontend/engine、空词表、非法几何或非法配置。
  // 成功后对象处于空闲新轮状态；调用方仍需先 set_callback 才能 feed。
  static domain::Result<std::unique_ptr<ZipformerStreamAsr>> create(
      std::unique_ptr<zipformer_detail::IFeatureFrontend> frontend,
      std::unique_ptr<zipformer_detail::IZipformerEngine> engine,
      std::vector<std::string> vocabulary,
      zipformer_detail::ZipformerStreamConfig config = {});

  ZipformerStreamAsr(const ZipformerStreamAsr&) = delete;
  ZipformerStreamAsr& operator=(const ZipformerStreamAsr&) = delete;
  ~ZipformerStreamAsr() override;

  // 注册空回调返回 kInvalidInput，保留旧回调与旧轮状态。成功注册是显式新轮边界：
  // 重置引擎缓存、特征前端、假设和取消标志；旧轮未提交的 partial/final 不会补发。
  domain::OperationResult set_callback(capability::TextEventCallback callback) override;

  // 取消优先于输入错误；非法帧/空回调返回 kInvalidInput 且不消费。成功送帧后按窗口
  // 同步执行推理并可能回调 partial；is_last=true 时最多补一次尾窗口并发 final+done。
  // 容量超限、引擎失败或 token 越界返回结构化错误，并把本轮标记为不可继续。
  domain::OperationResult feed(const domain::AudioFrame& frame, bool is_last) override;

  // 幂等、不分配、不阻塞、不抛异常。只置原子取消标志；正在执行的 feed 会在下一次
  // 窗口/token/事件边界观察它。返回后不会再开始新回调，但已进入的回调不会被撤回。
  domain::OperationResult cancel() noexcept override;

 private:
  struct Impl;
  explicit ZipformerStreamAsr(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::backend

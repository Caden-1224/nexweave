// MeloTTS 适配器的配置与统计值对象。
//
// 本文件只出现在 backend/适配器层，不包含 ONNX Runtime、RKNN 或任何厂商头文件，
// 也不把模型句柄、NPU context 或运行时线程暴露给 capability::ITts。配置路径由
// 调用方显式注入，适配器不根据当前工作目录或文件名猜测模型格式。
#pragma once

#include <cstdint>
#include <string>

namespace nexweave::backend {

// 编码器与解码器各自的运行方式。当前模型分工固定为：
//   - 编码器走 ONNX Runtime CPU，输入为 8 个 ONNX 张量；
//   - 解码器走 RKNN NPU，输入为 z_p 与 g 两个张量。
// 两个字段必须显式给出且不能混用，避免把 MeloTTS 误接成两套 .rknn 路径。
enum class MeloTtsRuntime {
  kOnnxRuntimeCpu,
  kRknnNpu,
};

// 真实 MeloTTS 适配器配置。所有路径在 factory 中校验可读性；数值上界在构造
// 适配器前检查，避免运行到一半才发现配置非法。
struct MeloTtsConfig {
  // 编码器 ONNX 模型路径；必须由 MeloTtsRuntime::kOnnxRuntimeCpu 加载。
  std::string encoder_model_path;
  // 解码器 RKNN 模型路径；必须由 MeloTtsRuntime::kRknnNpu 加载。
  std::string decoder_model_path;
  // 文本前端资源：词表、音素 token 与说话人风格 g 向量。
  // g 文件必须是 256 个 little-endian float32，共 1024 字节。
  std::string lexicon_path;
  std::string tokens_path;
  std::string g_path;

  // 显式声明两个模型的运行方式。只接受上面注释中的固定组合，其他组合返回
  // kInvalidInput，避免调用方把两个 ONNX 或两个 RKNN 误当成一条链路。
  MeloTtsRuntime encoder_runtime = MeloTtsRuntime::kOnnxRuntimeCpu;
  MeloTtsRuntime decoder_runtime = MeloTtsRuntime::kRknnNpu;

  // 模型原生输出采样率。当前导出件为 44100 Hz；适配器最终统一输出 16 kHz。
  std::uint32_t native_sample_rate_hz = 44100;
  // 语速与 VITS 推理参数。所有值必须为有限正数/有限非负数，具体校验见 factory。
  float speed = 0.8F;
  float noise_scale = 0.3F;
  float noise_scale_w = 0.6F;
  float sdp_ratio = 0.2F;
  // ONNX Runtime intra-op 线程数；1 表示推理线程只由当前调用线程承担。
  // RKNN 侧不创建线程，调用线程同步执行。
  int intra_op_num_threads = 1;

  // 单次 synthesize 的文本字节上限。超过上限显式返回 kInvalidInput，绝不截断后
  // 继续合成。默认 64 KiB 足以覆盖当前分句路径的单句输入。
  std::size_t max_text_bytes = 64U * 1024U;
  // 单个文本片段送入编码器的 interspersed phone 数上限。默认 240 与导出模型
  // 的序列长度预算兼容；文本前端按 unit 边界切分，保证不静默丢弃内容。
  std::size_t max_encoder_phones = 240U;
  // 单个文本片段允许的 z_p 帧数上限，用于约束编码器输出和解码器调用次数。
  // 默认 8192；超过时结构化失败，而不是继续分配无界内存。
  std::size_t max_z_frames_per_chunk = 8192U;
  // 单次 RKNN rknn_run 的有限等待毫秒数；0 表示不启用 SDK 超时。
  // 编码器为 ONNX Runtime CPU 同步调用，不受该值控制。
  std::uint32_t run_timeout_ms = 30000U;
};

// 一次 synthesize 的可审计统计。字段只在 synthesize 返回后读取；对象本身不是
// 线程安全容器，调用方必须在没有并发 synthesize/cancel 的线程上取快照。
struct MeloTtsStatistics {
  enum class TailKind : std::uint8_t {
    kNoOutput,     // 没有可交付的 16 kHz 样本。
    kFullFrame,    // 末尾有效样本正好填满 320 样本帧，没有补零。
    kPaddedTail,   // 末尾帧不足 320，补零到整帧后交付。
  };

  std::size_t input_text_bytes = 0;
  std::size_t text_chunks = 0;
  std::size_t unknown_units = 0;
  std::size_t encoder_runs = 0;
  std::size_t decoder_runs = 0;
  // 编码器输出的 z_p 帧总数对应的原生 float 样本数（未重采样）。
  std::size_t native_samples = 0;
  // 重采样后、补零前实际有效的 16 kHz 样本数。它不是 native_samples/2.75625
  // 的近似值，而是线性重采样器按输入时长确定性计算出的有效样本计数。
  std::size_t resampled_samples = 0;
  std::size_t delivered_frames = 0;
  // 末尾帧有效样本数：tail_kind == kFullFrame 时为 320，kPaddedTail 时为 1..319，
  // kNoOutput 时为 0。补零样本不计入该值，也不计入 resampled_samples。
  std::size_t last_frame_valid_samples = 0;
  TailKind tail_kind = TailKind::kNoOutput;
};

}  // namespace nexweave::backend

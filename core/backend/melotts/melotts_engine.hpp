// 文本前端与推理引擎之间的最小接口。
//
// 这些接口只存在于 backend/melotts 适配器层：核心 Session 看不到 z_p、RKNN
// context 或 ONNX Runtime 张量。把编码器和解码器拆成接口后，WSL 默认构建可用
// 可控 Fake 引擎验证合成状态机、重采样和取消，真实硬件路径仅在可选 CMake 开关
// 打开时编译。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../domain/error.hpp"

namespace nexweave::backend {

// 编码器输入。所有指针只在 run 调用期间有效；实现必须复制它需要异步保留的数据。
// phone/tone/language 三个数组长度必须相同，g_count 必须与模型要求一致。
struct MeloEncoderRequest {
  const std::int32_t* phones = nullptr;
  std::size_t phone_count = 0;
  const std::int32_t* tones = nullptr;
  std::size_t tone_count = 0;
  const std::int32_t* languages = nullptr;
  std::size_t language_count = 0;
  const float* g = nullptr;
  std::size_t g_count = 0;
  float noise_scale = 0.0F;
  float noise_scale_w = 0.0F;
  float length_scale = 1.0F;
  float sdp_ratio = 0.0F;
};

// 编码器输出。z_p 按 [channels][frames] 连续存放；调用方不得假设 frames 不超过
// 某固定值，必须使用实际值。phone_lengths 与输入 phone_count 等长，audio_len_samples
// 是模型给出的原生采样数，用于交叉校验解码器帧覆盖范围。
struct MeloEncoderResponse {
  std::vector<float> z_p;
  std::size_t channels = 0;
  std::size_t frames = 0;
  std::vector<std::int32_t> phone_lengths;
  std::int32_t audio_len_samples = 0;
};

// 文本到声学编码器。接口不声明取消；实现是同步调用，适配器只能在该调用返回后
// 检查原子取消标志，并在文档中说明最长不可中断窗口。
class IMeloEncoder {
 public:
  virtual ~IMeloEncoder() = default;
  virtual domain::OperationResult run(const MeloEncoderRequest& request,
                                      MeloEncoderResponse& response) = 0;
};

// 解码器固定形状信息。frames_per_call 是单次解码器调用接受的 z_p 帧数，
// samples_per_frame 是每个 z_p 帧对应的原生 float 样本数。
struct MeloDecoderInfo {
  std::size_t channels_per_frame = 0;
  std::size_t frames_per_call = 0;
  std::size_t samples_per_frame = 0;
};

// 声学到波形解码器。decode 从 z_p[frame_offset..frame_offset+frame_count) 生成
// 原生 float 样本；frame_count 必须不超过 frames_per_call，输出写入 audio 并
// 恰好包含 frame_count * samples_per_frame 个样本。实现负责对不足
// frames_per_call 的输入做零填充，但不得把补出来的帧计入返回值。
class IMeloDecoder {
 public:
  virtual ~IMeloDecoder() = default;
  virtual const MeloDecoderInfo& info() const noexcept = 0;
  virtual domain::OperationResult decode(const float* z_p,
                                         std::size_t total_z_frames,
                                         std::size_t frame_offset,
                                         std::size_t frame_count,
                                         std::vector<float>& audio) = 0;
};

}  // namespace nexweave::backend

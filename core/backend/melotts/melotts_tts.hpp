// MeloTTS 合成适配器：组合文本前端、ONNX/RKNN 推理接口、流式重采样与 ITts 状态机。
//
// Session 只通过 capability::ITts 使用本对象；MeloPhoneChunk、z_p、ONNX Runtime
// 和 RKNN 类型都留在 backend/melotts 内。对象不创建后台线程：编码器/解码器在
// synthesize 的调用线程同步执行，PCM 回调也在同一线程逐帧发生，因此首块 PCM
// 可以在整个文本合成完成前交给接收方。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../../capability/backend.hpp"
#include "melotts_engine.hpp"
#include "melotts_text_frontend.hpp"
#include "melotts_types.hpp"

namespace nexweave::backend {

// 合成参数。g 是模型要求的说话人风格向量，由 factory 从 g_path 读取后传入；
// 本结构不拥有文件句柄或 SDK 资源。所有阈值都显式可配置，构造时校验。
struct MeloTtsSynthesisOptions {
  std::vector<float> g;
  std::uint32_t native_sample_rate_hz = 44100;
  float speed = 0.8F;
  float noise_scale = 0.3F;
  float noise_scale_w = 0.6F;
  float sdp_ratio = 0.2F;
  std::size_t max_text_bytes = 64U * 1024U;
  std::size_t max_encoder_phones = 240U;
  std::size_t max_z_frames_per_chunk = 8192U;
};

// 真实合成器与可控 Fake 引擎共用的实现。它实现 ITts 的同步逐帧回调、回调异常
// 封锁、并发取消检查、流式重采样和尾部补零。
//
// 资源所有权：create 按值接收 encoder/decoder/frontend，成功时移入对象，析构负责
// 释放；失败路径的参数已经移动，会随形参析构，因此失败也不会丢弃已加载资源。
// 线程：set_callback/synthesize 由调用方串行调用；cancel 可与 synthesize 并发，
// 仅访问原子标志。回调在 synthesize 的调用线程执行，接收方必须复制帧。
class MeloTtsTts final : public capability::ITts {
 public:
  // 创建适配器。encoder/decoder 必须非空，frontend 必须已加载成功(loaded_ 为真),
  // 参数、采样率、g 大小和 decoder info 都在这里校验；成员加载失败返回结构化错误。
  static domain::Result<std::unique_ptr<MeloTtsTts>> create(
      std::unique_ptr<IMeloEncoder> encoder,
      std::unique_ptr<IMeloDecoder> decoder,
      MeloTextFrontend frontend,
      MeloTtsSynthesisOptions options);

  MeloTtsTts(const MeloTtsTts&) = delete;
  MeloTtsTts& operator=(const MeloTtsTts&) = delete;
  ~MeloTtsTts() override = default;

  domain::OperationResult set_callback(capability::AudioEventCallback callback) override;
  domain::OperationResult synthesize(const std::string& text) override;
  domain::OperationResult cancel() noexcept override;

  // 返回最近一次 synthesize 的统计快照。只能在 synthesize 返回后、且没有并发
  // synthesize/cancel 时读取；对象不保证对内部统计做额外同步。
  MeloTtsStatistics last_statistics() const noexcept { return statistics_; }

 private:
  MeloTtsTts(std::unique_ptr<IMeloEncoder> encoder,
             std::unique_ptr<IMeloDecoder> decoder,
             MeloTextFrontend frontend,
             MeloTtsSynthesisOptions options);

  domain::OperationResult run_synthesis(const std::string& text);
  // 把 frame_buffer 中已有的完整 320 样本帧交付；flush_tail 为 true 时把不足
  // 320 的尾帧补零后交付，并更新尾部统计。返回 false 表示取消。
  bool emit_frames(std::vector<std::int16_t>& frame_buffer, bool flush_tail);

  std::unique_ptr<IMeloEncoder> encoder_;
  std::unique_ptr<IMeloDecoder> decoder_;
  MeloTextFrontend frontend_;
  MeloTtsSynthesisOptions options_;
  capability::AudioEventCallback callback_;
  std::atomic<bool> cancelled_{false};
  MeloTtsStatistics statistics_{};
};

}  // namespace nexweave::backend

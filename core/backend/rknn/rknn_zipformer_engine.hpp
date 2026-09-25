// RKNN Zipformer 三模型引擎的私有接口。
//
// 本头文件只出现在 backend 适配器层，不包含 rknn_api.h，也不让 RKNN 上下文、
// 张量属性或 NPU 句柄进入 IAsr / Session。具体实现文件负责加载 encoder/decoder/joiner
// 三个 .rknn，维护 encoder 的跨窗口缓存，并把 RKNN 错误映射为统一错误码。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "../zipformer_stream.hpp"

namespace nexweave::backend {

// 真实 RKNN 引擎的构造参数。路径由调用方注入，不得依赖当前工作目录。
struct RknnZipformerEngineOptions {
  std::string encoder_model_path;
  std::string decoder_model_path;
  std::string joiner_model_path;
  std::size_t blank_token_id = 0;
  std::size_t unk_token_id = 2;
  // 单次 rknn_run 的有限等待。0 表示不依赖 SDK 超时，由外层取消和进程隔离负责；
  // 默认 30000 ms。该值只在引擎内部使用，不改变 IAsr 的同步回调契约。
  std::uint32_t run_timeout_ms = 30000;
  // 当前导出模型使用 4 倍时间下采样；该值必须与模型几何一致，否则窗口步长校验失败。
  std::size_t encoder_subsampling_factor = 4;
  // fbank 帧移对应原始采样数，10 ms @ 16 kHz 为 160。
  std::size_t feature_frame_shift_samples = 160;
};

// 三模型 RKNN 推理引擎。实现 IZipformerEngine 的窗口接口，隐藏 encoder 缓存复制、
// FP16/INT64 缓冲区、joiner argmax 输入和错误映射。对象拥有三个 RKNN context 及其
// 输入/输出缓冲区；析构释放全部 NPU 资源。所有方法由适配器核心串行调用，不创建线程。
class RknnZipformerEngine final : public zipformer_detail::IZipformerEngine {
 public:
  // 加载并校验三个模型；任一模型失败都释放已加载资源并返回结构化错误。
  static domain::Result<std::unique_ptr<RknnZipformerEngine>> create(
      const RknnZipformerEngineOptions& options);

  RknnZipformerEngine(const RknnZipformerEngine&) = delete;
  RknnZipformerEngine& operator=(const RknnZipformerEngine&) = delete;
  ~RknnZipformerEngine() override;

  const zipformer_detail::ZipformerModelInfo& info() const noexcept override;
  domain::OperationResult reset_round() override;
  domain::OperationResult encode_chunk(const float* features,
                                       std::size_t frame_count,
                                       std::vector<float>& encoder_output) override;
  domain::OperationResult decode(const std::int64_t* context,
                                 std::size_t context_size,
                                 std::vector<float>& decoder_output) override;
  domain::OperationResult join(const float* encoder_output,
                               const float* decoder_output,
                               std::vector<float>& logits) override;

  // 第一次成功加载 encoder 后查询到的 SDK 版本；用于运行证据，不参与业务分支。
  std::string sdk_api_version() const;
  std::string sdk_driver_version() const;

 private:
  struct Impl;
  explicit RknnZipformerEngine(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::backend

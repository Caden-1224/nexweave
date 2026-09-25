// MeloTTS 真实推理引擎的 adapter 层声明。
//
// 本头文件不包含 onnxruntime_c_api.h 或 rknn_api.h，因此硬件类型也不会通过
// MeloTtsTts 头泄漏给其它模块。只有打开可选 CMake 开关的源文件包含实现。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "../../domain/error.hpp"
#include "melotts_engine.hpp"

namespace nexweave::backend {

struct MeloOrtEncoderConfig {
  std::string model_path;
  int intra_op_num_threads = 1;
};

// ONNX Runtime CPU 编码器。对象拥有 OrtEnv/OrtSession 及其张量内存；析构释放
// session、options、env。run 是同步调用，不创建后台线程。
class MeloOrtEncoder final : public IMeloEncoder {
 public:
  static domain::Result<std::unique_ptr<MeloOrtEncoder>> create(
      const MeloOrtEncoderConfig& config);
  ~MeloOrtEncoder() override;

  MeloOrtEncoder(const MeloOrtEncoder&) = delete;
  MeloOrtEncoder& operator=(const MeloOrtEncoder&) = delete;

  domain::OperationResult run(const MeloEncoderRequest& request,
                              MeloEncoderResponse& response) override;

 private:
  struct Impl;
  explicit MeloOrtEncoder(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

struct MeloRknnDecoderConfig {
  std::string model_path;
  std::uint32_t run_timeout_ms = 30000U;
};

// RKNN NPU 解码器。对象拥有 rknn_context、输入输出缓冲区和模型属性；析构按
// 先释放 outputs（若有）、再销毁 context 的顺序释放。decode 同步调用 rknn_run，
// 使用配置的有限超时，不创建后台线程。
class MeloRknnDecoder final : public IMeloDecoder {
 public:
  static domain::Result<std::unique_ptr<MeloRknnDecoder>> create(
      const MeloRknnDecoderConfig& config);
  ~MeloRknnDecoder() override;

  MeloRknnDecoder(const MeloRknnDecoder&) = delete;
  MeloRknnDecoder& operator=(const MeloRknnDecoder&) = delete;

  const MeloDecoderInfo& info() const noexcept override;
  domain::OperationResult decode(const float* z_p,
                                 std::size_t total_z_frames,
                                 std::size_t frame_offset,
                                 std::size_t frame_count,
                                 std::vector<float>& audio) override;

 private:
  struct Impl;
  explicit MeloRknnDecoder(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::backend

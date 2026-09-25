// RKNN Zipformer ASR 适配器的公共构造入口。
//
// 适配器实现 capability::IAsr：Session 只看到逐帧 feed、partial/final/done 与 cancel，
// 看不到 RKNN context、张量、动态库路径或 kaldi fbank 类型。模型文件、词表和容量由
// RknnZipformerConfig 显式注入；构造函数不依赖当前工作目录。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "../../capability/backend.hpp"

namespace nexweave::backend {

// RKNN Zipformer 适配器配置。所有路径在 create 时校验；容量和超时必须有明确正值。
struct RknnZipformerConfig {
  // 三个 .rknn 与 vocab.txt 的绝对路径或相对调用方工作目录的路径。适配器自身不拼接
  // 模型目录，避免把板端部署布局写死在核心代码里。
  std::string encoder_model_path;
  std::string decoder_model_path;
  std::string joiner_model_path;
  std::string vocab_path;
  // 当前词表约定：blank=0、unk=2；改变前必须同步核对模型输出类别和词表索引。
  std::size_t blank_token_id = 0;
  std::size_t unk_token_id = 2;
  // 单轮原始音频上限，默认 10 分钟；超限返回 kBackendFailure。
  std::size_t max_round_samples = 16000U * 600U;
  // 单轮 token 上限，默认 20000；超限返回 kBackendFailure。
  std::size_t max_hypothesis_tokens = 20000U;
  // 结束刷新最多补几轮零样本窗口；默认 1，仅用于形成最后一次 encoder 窗口。
  std::size_t max_tail_pad_chunks = 1U;
  // 单次 rknn_run 有限等待；默认 30000 ms。0 表示不启用 SDK 超时。
  std::uint32_t run_timeout_ms = 30000;
};

// 真实 RKNN Zipformer ASR。对象拥有三个 RKNN 模型、encoder 跨窗口缓存、在线 fbank
// 和适配器核心状态；析构按拥有的顺序释放 NPU 与特征资源。所有 IAsr 方法语义沿用
// 公共能力契约：调用方串行 set_callback/feed，cancel 可来自控制线程且非阻塞。
class RknnZipformerAsr final : public capability::IAsr {
 public:
  // 加载词表、三个模型并建立空闲新轮对象。任一模型/词表失败都返回结构化错误并释放
  // 已成功加载的资源；成功后仍需 set_callback 才能 feed。
  static domain::Result<std::unique_ptr<RknnZipformerAsr>> create(
      const RknnZipformerConfig& config);

  RknnZipformerAsr(const RknnZipformerAsr&) = delete;
  RknnZipformerAsr& operator=(const RknnZipformerAsr&) = delete;
  ~RknnZipformerAsr() override;

  domain::OperationResult set_callback(capability::TextEventCallback callback) override;
  domain::OperationResult feed(const domain::AudioFrame& frame, bool is_last) override;
  domain::OperationResult cancel() noexcept override;

  // 实际加载 encoder 时的 RKNN Runtime 版本；用于运行清单和板端证据。返回值是副本，
  // 调用方可在对象生命周期内任意使用。
  std::string sdk_api_version() const;
  std::string sdk_driver_version() const;

 private:
  struct Impl;
  explicit RknnZipformerAsr(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::backend

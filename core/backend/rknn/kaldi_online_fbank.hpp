// 在线 fbank 特征前端：把 16 kHz 单声道 S16_LE 样本转换为 Zipformer 需要的 80 维
// kaldi fbank 特征。kaldi-native-fbank 类型只存在于实现文件；本头文件保持为
// IFeatureFrontend 的一个可选实现，不暴露第三方 SDK 类型。
#pragma once

#include <memory>

#include "../zipformer_stream.hpp"

namespace nexweave::backend {

// 当前 RKNN Zipformer 导出使用 10 ms 帧移、25 ms 窗、80 维 mel、high_freq=-400、
// 无 dither、snip_edges=false。这些参数与训练导出保持一致，不开放任意覆盖；需要改变
// 时必须同步重新验证模型输入窗口和识别结果。对象拥有 OnlineFbank 窗口，析构释放其内存。
class KaldiOnlineFbankFrontend final : public zipformer_detail::IFeatureFrontend {
 public:
  KaldiOnlineFbankFrontend(const KaldiOnlineFbankFrontend&) = delete;
  KaldiOnlineFbankFrontend& operator=(const KaldiOnlineFbankFrontend&) = delete;
  ~KaldiOnlineFbankFrontend() override;

  // 创建失败只会发生在参数或内存初始化异常；异常向上传播，调用方负责失败清理。
  static std::unique_ptr<KaldiOnlineFbankFrontend> create();

  void reset() override;
  domain::OperationResult accept(const std::int16_t* samples,
                                 std::size_t count) override;
  domain::OperationResult append_silence(std::size_t count) override;
  std::size_t available_feature_frames() const override;
  const float* feature_frame(std::size_t absolute_index) const override;
  domain::OperationResult discard_first(std::size_t frame_count) override;

 private:
  struct Impl;
  KaldiOnlineFbankFrontend();

  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::backend

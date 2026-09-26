// WebRTC 音频前处理器：把板端可加载的系统库封装为 runtime::IAudioProcessor。
//
// 职责
// ----
// 本类只负责固定 16 kHz/单声道/S16_LE/160 样本窗口的 AEC 与 WebRTC 降噪，
// 不打开 ALSA 设备、不解析 20 ms 领域帧、不持有播放参考时间轴。渲染参考与
// 近端帧均由 runtime::DuplexAudioFrontend 按时间槽传入。
//
// 设备与库假设
// ------------
// 真实实现只在 NEXWEAVE_ENABLE_WEBRTC_APM=ON 时编译，链接板端已实测可加载的
// libwebrtc_audio_processing。源码不硬编码设备名或绝对库路径；库路径由 CMake
// 配置和板端入口显式提供。头文件不包含 WebRTC 类型，避免把旧版 C ABI 泄漏到
// 核心层。
//
// 状态与资源
// ----------
// open() 创建 AEC 与 NS 句柄，close() 释放它们；reset() 重新创建句柄并清空
// 算法状态，但保留累计计数和错误。任何处理失败都会把状态置为 kFailed，调用方
// 必须显式 reset()/close() 后重新 open()。AEC 收敛状态依据 delay 指标、处理
// 窗口数和可选 ERLE 阈值判定，设备能打开或库能加载都不等于 AEC 已收敛。
#pragma once

#include <memory>

#include "../../runtime/audio_processor.hpp"

namespace nexweave::backend {

class WebrtcAudioProcessor final : public runtime::IAudioProcessor {
 public:
  explicit WebrtcAudioProcessor(runtime::AudioProcessorConfig config = {});
  ~WebrtcAudioProcessor() override;

  WebrtcAudioProcessor(const WebrtcAudioProcessor&) = delete;
  WebrtcAudioProcessor& operator=(const WebrtcAudioProcessor&) = delete;

  domain::OperationResult open() override;
  domain::OperationResult process_render(
      const runtime::AudioProcessorFrame& frame) override;
  domain::Result<runtime::AudioProcessorFrame> process_capture(
      const runtime::AudioProcessorFrame& frame) override;
  domain::OperationResult reset(const std::string& reason) override;
  domain::OperationResult close() noexcept override;
  runtime::AudioProcessorState state() const override;
  runtime::AudioProcessorStats stats() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::backend

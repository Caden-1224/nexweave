// ALSA PCM 采集后端：PcmCaptureBackend 的真实设备实现。
//
// 该头文件只在 NEXWEAVE_ENABLE_ALSA=ON 构建中参与编译，默认 WSL 构建不接触
// libasound。snd_pcm_t 和 ALSA 头只出现在实现文件由 pimpl 隔离；公开接口仍是
// PcmCaptureBackend 的 NexWeave 值类型，因此 Session 与核心层看不到设备句柄。
//
// 资源与线程：对象创建时不打开设备、不创建线程；open() 打开并准备 PCM 句柄，
// close() 释放。read() 由适配器设备互斥量串行调用，内部按配置的 poll slice 分段
// 等待，cancel() 设置原子取消标志，最迟在一个 slice 后让 read() 退出。析构会
// 再次 close()，但调用方仍需保证 read() 已退出后再销毁对象。
#pragma once

#include <memory>

#include "pcm_capture_backend.hpp"

namespace nexweave::backend {

class AlsaPcmCaptureBackend final : public PcmCaptureBackend {
 public:
  // 构造只保存配置，不打开设备；配置在 open() 时通过
  // validate_pcm_capture_backend_config() 校验。
  explicit AlsaPcmCaptureBackend(PcmCaptureBackendConfig config);
  ~AlsaPcmCaptureBackend() override;

  AlsaPcmCaptureBackend(const AlsaPcmCaptureBackend&) = delete;
  AlsaPcmCaptureBackend& operator=(const AlsaPcmCaptureBackend&) = delete;

  domain::OperationResult open() override;
  PcmCaptureReadResult read(std::chrono::milliseconds timeout) override;
  domain::OperationResult recover() override;
  domain::OperationResult cancel() noexcept override;
  domain::OperationResult close() noexcept override;
  PcmCaptureFormat current_format() const noexcept override;
  PcmCaptureBackendStats stats() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::backend

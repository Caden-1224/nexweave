// ALSA PCM 播放后端：PcmPlaybackBackend 的真实设备实现。
//
// 该头文件只在 NEXWEAVE_ENABLE_ALSA=ON 构建中参与编译，默认 WSL 构建不接触
// libasound。snd_pcm_t 和 ALSA 头只出现在实现文件由 pimpl 隔离；公开接口仍是
// PcmPlaybackBackend 的 NexWeave 值类型。
//
// 资源与线程：构造不打开设备、不创建线程；open() 打开并准备 PCM 播放句柄，
// close() 释放。write/drain 由适配器设备互斥量串行调用；cancel() 只写原子标志
// 并在获得设备锁后 drop，从而有限唤醒等待可写空间的 write。正常关闭由适配器在
// drain() 成功后再 close()；取消后的 close 走 drop 路径。
#pragma once

#include <memory>

#include "pcm_playback_backend.hpp"

namespace nexweave::backend {

class AlsaPcmPlaybackBackend final : public PcmPlaybackBackend {
 public:
  explicit AlsaPcmPlaybackBackend(PcmPlaybackBackendConfig config);
  ~AlsaPcmPlaybackBackend() override;

  AlsaPcmPlaybackBackend(const AlsaPcmPlaybackBackend&) = delete;
  AlsaPcmPlaybackBackend& operator=(const AlsaPcmPlaybackBackend&) = delete;

  domain::OperationResult open() override;
  PcmPlaybackWriteResult write(const std::int32_t* interleaved_samples,
                               std::size_t frames,
                               std::chrono::milliseconds timeout) override;
  domain::OperationResult recover() override;
  domain::OperationResult drain(std::chrono::milliseconds timeout) override;
  domain::OperationResult cancel() noexcept override;
  domain::OperationResult close() noexcept override;
  PcmPlaybackFormat current_format() const noexcept override;
  PcmPlaybackBackendStats stats() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::backend

// WSL/Mock 阶段的确定性音频输入输出实现。
//
// Fake 只实现 capability::IAudioSource/IAudioSink 的值语义，不访问声卡、文件、
// socket 或模型运行库。它把一段由调用方提供的 PCM 样本按统一的 20 ms 帧合同
// 分发给 Session，并把已写入的帧保存在内存快照中，供测试和 Mock profile 校验。
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../capability/backend.hpp"

namespace nexweave::backend {

/**
 * 确定性内存音频源。
 *
 * 输入前提：构造参数必须是 16 kHz、单声道、S16_LE PCM 样本，并且样本数是
 * 320（20 ms）帧长度的整数倍；空输入或尾部残帧会在 open() 返回 kInvalidInput，
 * 不会被补零、截断或重采样。构造函数只复制/移动样本，不创建外部资源。
 * 输出后置：每次成功 read() 返回下一帧的独立 AudioFrame；读完后返回
 * kAlreadyCompleted，且不再产生伪造帧。close() 后 read() 返回 kDeviceFailure。
 * cancel() 是取消线性化点：从该点起 read() 返回 kCancelled；再次 open() 会把
 * 游标归零并清除取消状态，从而同一输入可以重复、确定性地运行。
 * 并发安全：open/close/read/cancel 由同一互斥量线性化；read 不阻塞外部资源，
 * 取消与读竞争时以先取得锁的操作为准。对象不拥有后台线程，析构无需额外 join。
 */
class FakeAudioSource final : public capability::IAudioSource {
 public:
  explicit FakeAudioSource(std::vector<std::int16_t> pcm_samples);

  domain::OperationResult open() override;
  domain::Result<domain::AudioFrame> read() override;
  domain::OperationResult close() noexcept override;

  // 取消当前输入代际；幂等且不清除源数据，供下一次 open() 重放同一夹具。
  domain::OperationResult cancel() noexcept;

  // 返回总帧数和已消费帧数的快照，便于测试验证没有越界或重复消费。
  std::size_t frame_count() const noexcept;
  std::size_t frames_read() const noexcept;

 private:
  std::vector<std::int16_t> pcm_samples_;
  mutable std::mutex mutex_;
  std::size_t next_sample_ = 0;
  bool opened_ = false;
  bool cancelled_ = false;
};

/**
 * 确定性内存音频汇。
 *
 * open() 是一次运行的边界：从关闭状态打开时清空上一轮帧，防止重复运行的
 * 输出残留；已打开时返回 kAlreadyCompleted 且不清空当前结果。write() 只接受
 * validate_audio_frame() 成功的固定帧，成功后复制样本，调用方可立即复用输入。
 * 未打开写入返回 kDeviceFailure，取消后返回 kCancelled；cancel() 会丢弃当前
 * 运行已缓存的帧，避免被取消的旧代输出泄漏到下一轮。close() 幂等且保留最后
 * 一轮结果供测试读取，不创建文件、设备句柄或线程。
 * 并发安全：状态和帧缓存由互斥量保护，frames()/pcm_samples() 返回独立快照；
 * write 与 cancel 的先后以锁内线性化顺序决定，调用方无需管理内部资源。
 */
class FakeAudioSink final : public capability::IAudioSink {
 public:
  FakeAudioSink() = default;

  domain::OperationResult open() override;
  domain::OperationResult write(const domain::AudioFrame& frame) override;
  domain::OperationResult close() noexcept override;

  // 取消当前输出代际并清理已缓存帧；可重复调用且不影响下一次 open()。
  domain::OperationResult cancel() noexcept;

  // 返回最近一次运行的帧/PCM 独立快照；调用不改变 open/close/cancel 状态。
  std::vector<domain::AudioFrame> frames() const;
  std::vector<std::int16_t> pcm_samples() const;

 private:
  mutable std::mutex mutex_;
  std::vector<domain::AudioFrame> frames_;
  bool opened_ = false;
  bool cancelled_ = false;
};

}  // namespace nexweave::backend

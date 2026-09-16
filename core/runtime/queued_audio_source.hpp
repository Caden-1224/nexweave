// 队列驱动的音频源：把“网络或上层逐帧推送 PCM”适配成 capablity::IAudioSource。
//
// 职责与适用范围
// --------------
// 本类是远端会话输入侧的唯一音频源实现：生产者线程把来自传输层的完整 20 ms 帧放入有界
// 队列，会话线程按常驻输入契约逐帧 read()。它不解析网络、不做分帧、不判断语音活动，
// 也不拥有会话或设备；这些职责分别由远端会话适配器、ResidentAudioInput 与判定器承担。
//
// 线程与所有权
// ------------
// push/end_input/cancel/close 由生产侧或收尾线程调用；read 由唯一的会话消费线程调用。
// 队列、终态与统计由同一把互斥量保护，条件变量只负责唤醒阻塞的 read，不改变调用顺序。
// 本对象不创建线程、socket、文件或设备句柄；帧副本由队列拥有，析构即释放。
//
// 失败与清理
// ----------
// 满队列不阻塞、不覆盖旧帧，push 返回 kBackendFailure 并拒绝该帧，调用方必须显式处理。
// cancel 清空尚未消费的帧并唤醒 read；read 在取消后返回 kCancelled，保证退出不会卡在
// 等帧上。end_input 保留已经入队的完整帧，消费完再返回 kAlreadyCompleted，用于表达
// “输入自然结束”。close 幂等，关闭后 read/push 明确失败。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "../capability/backend.hpp"

namespace nexweave::runtime {

// 队列音频源配置。容量单位是完整音频帧；取 0 非法，构造时整份回退默认值。
struct QueuedAudioSourceConfig {
  // 尚未被会话消费的帧上限。它直接约束远端会话入口的内存占用；慢消费时生产者会得到
  // 结构化失败，而不是把队列撑到无界。
  std::size_t max_pending_frames = 64;
};

// 配置校验：只读、不分配、不启动线程。容量为 0 返回 kInvalidInput。
domain::OperationResult validate_queued_audio_source_config(
    const QueuedAudioSourceConfig& config);

// 只读账目快照。字段只增不减，用于证明生产、消费、拒绝被分别记录。
struct QueuedAudioSourceStats {
  std::uint64_t pushed = 0;
  std::uint64_t popped = 0;
  std::uint64_t rejected_full = 0;
  std::uint64_t cancelled_reads = 0;
};

// 有界且可取消的音频源。调用方必须先 open()（或先 push，open 后消费），再按音频合同
// 推送完整帧；read 不返回短帧、不补零、不重采样。
class QueuedAudioSource final : public capability::IAudioSource {
 public:
  explicit QueuedAudioSource(QueuedAudioSourceConfig config = {});
  ~QueuedAudioSource() override = default;

  QueuedAudioSource(const QueuedAudioSource&) = delete;
  QueuedAudioSource& operator=(const QueuedAudioSource&) = delete;

  // 打开一个输入轮次：清除取消标志与上一轮残留队列，进入可读状态。首次调用成功；
  // 已打开时返回 kAlreadyCompleted，不重复清空已经到达的帧。close 后可以重新 open。
  domain::OperationResult open() override;

  // 阻塞读取下一帧。未 open、已 close 或已取消时分别返回 kDeviceFailure/kCancelled；
  // 队列为空且输入尚未结束时等待条件变量，不轮询、不读时钟。cancel/end_input/close
  // 都能唤醒等待。
  domain::Result<domain::AudioFrame> read() override;

  // 取消当前轮次：丢弃尚未消费的帧并唤醒 read。幂等、不抛异常，后续 read 返回 kCancelled；
  // 已经交付给调用方的帧不撤回。
  domain::OperationResult cancel() noexcept override;

  // 关闭音频源并释放队列；幂等且不抛异常。关闭后 push/read 明确失败，重新 open 可复用
  // 同一对象但不会恢复上一轮的帧。
  domain::OperationResult close() noexcept override;

  // 生产侧接口：把一帧放入队列。未 open 也允许入队，以消除“传输先到、会话后启动”的
  // 启动竞态；但 close/cancel/end_input 之后拒绝。队列满返回 kBackendFailure 且不丢旧帧。
  domain::OperationResult push(domain::AudioFrame frame);

  // 标记输入自然结束：保留已排队帧，read 消费完后返回 kAlreadyCompleted。重复调用幂等；
  // 已经取消或关闭时返回成功但不改变终态。
  domain::OperationResult end_input() noexcept;

  // 当前是否仍有可读帧；供测试与运行证据核对，不改变状态。
  bool has_pending_frames() const noexcept;

  // 队列与拒绝计数快照。
  QueuedAudioSourceStats stats() const noexcept;

 private:
  // 记录一次取消读取；在持锁路径上调用，避免统计与终态读取不一致。
  void note_cancelled_read_locked();

  QueuedAudioSourceConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<domain::AudioFrame> pending_;
  bool opened_ = false;
  bool closed_ = false;
  bool ended_ = false;
  bool cancelled_ = false;
  QueuedAudioSourceStats stats_{};
};

}  // namespace nexweave::runtime

// ALSA 音频输出适配器：把真实播放后端转换为统一 capability::IAudioSink。
//
// 职责
// ----
// 本类只负责一次播放轮次的 open/close、把统一的 16 kHz/单声道/S16_LE/320 样本帧
// 显式转换为设备原生采样率/声道/编码、处理短写、underrun、超时、取消、断开重连
// 和正常排空；同时把“实际成功写入设备”的原生样本、顺序号和时间信息通过可选
// 回放参考回调交给后续音频前端。它不计算语音活动、不做回声消除、不拥有双向设备。
//
// 设备边界
// --------
// 设备名、原生采样率、声道、编码、period/buffer 只存在于 PcmPlaybackBackend 及
// 其配置中；Session 和核心契约看不到 snd_pcm_t。适配器不硬编码板端设备名，也不
// 假设设备原生支持 16 kHz 单声道 S16_LE：实际格式由后端 open 时回报，转换器按
// 实际值显式重采样、复制声道并转换样本编码。
//
// 播放与取消
// ----------
// write 成功只表示整帧已被设备接受，不表示已经播完；close 正常路径先排空设备
// 缓冲再释放句柄。cancel 丢弃设备中尚未播放的软件/设备可丢弃数据，并丢弃适配器
// 未写出的转换缓存；已经物理播放的声音不可撤回。取消后必须 close/open 才能开始
// 下一轮，重复 open 不会清除取消状态。
//
// 回放参考
// --------
// 参考块只在后端确实接受原生样本后产生，包含实际样本副本、actual format、序列号、
// 写入前后时间点、设备剩余延迟帧数以及连续标志。underrun/断开恢复/取消/关闭会使
// 下一段参考明确标为不连续或产生不连续事件；这些信息供任务 46 建立 AEC 回放参考
// 对齐，不能在 Session 中直接使用。
//
// 失败与清理
// ----------
// 所有等待、重试、排空和恢复预算都来自 AlsaAudioSinkConfig 并经过校验。write
// 超时返回 kTimeout；设备断开在 reconnect_budget 内调用后端 recover()，失败或
// 预算耗尽返回 kDeviceFailure；短写按接受帧数推进，不重复写已接受数据。close
// 幂等、不抛异常；取消后的 close 走丢弃路径，不会重新播放已丢弃的数据。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../capability/backend.hpp"
#include "pcm_playback_backend.hpp"

namespace nexweave::backend {

// 适配器策略配置。设备原生参数在 PcmPlaybackBackendConfig 中显式配置并校验；
// 本结构只约束一次 write、取消和正常关闭的等待预算，避免把设备参数与操作策略
// 混成一个含义模糊的结构。
struct AlsaAudioSinkConfig {
  // 一次 capability::IAudioSink::write() 从开始到成功/失败的最长等待。重采样后
  // 一帧可能拆成多次后端写入，因此这是整个 write 调用的上限。必须大于 0。
  std::chrono::milliseconds write_timeout{500};

  // 一次 write 最多调用后端多少次。它防止后端连续返回短写/恢复状态时形成空转；
  // 达到上限返回 kTimeout/kDeviceFailure，而不是无限重试。
  std::size_t max_backend_writes_per_frame = 64;

  // 一次 write 内允许发起多少次设备重连。每次重连会调用后端 recover()，并把
  // 回放参考标记为不连续。0 表示不重连。
  std::size_t reconnect_budget = 2;

  // 一次 write 内允许处理多少次底层 underrun/挂起恢复。与重连预算分开，因为
  // underrun 通常不需要关闭设备，但连续恢复同样必须有上限。
  std::size_t max_recoveries_per_frame = 8;

  // 正常 close 时等待设备缓冲排空的上限；超时后丢弃剩余设备缓冲并返回 kTimeout。
  // 取消后的 close 不进入排空路径。必须大于 0。
  std::chrono::milliseconds drain_timeout{5000};
};

// 回放参考块类型。kAudio 表示后续 samples 是设备实际接受的原生样本；kDiscontinuity
// 表示参考时间轴出现缺口，samples 为空，接收方必须重置对齐状态。
enum class AlsaPlaybackReferenceKind : std::uint8_t {
  kAudio,
  kDiscontinuity
};

// 实际成功写入设备的回放参考。samples 使用原生交错 int32_t 表示；音量和内容与
// 设备接受的信号一致，并附带实际格式和时间信息。对象拥有样本副本，回调返回后
// 调用方不能再依赖引用。
struct AlsaPlaybackReferenceBlock {
  AlsaPlaybackReferenceKind kind = AlsaPlaybackReferenceKind::kAudio;
  PcmPlaybackFormat format;
  std::vector<std::int32_t> interleaved_samples;
  // 同一打开轮次内单调递增的参考序号；从 0 开始。
  std::uint64_t sequence = 0;
  // 后端开始/完成接受本块数据的 steady_clock 时间戳，单位纳秒；-1 表示不可用。
  std::int64_t write_begin_steady_ns = -1;
  std::int64_t write_done_steady_ns = -1;
  // 写入完成后设备仍待播放的估计帧数；-1 表示后端不可查询。
  std::int64_t delay_frames = -1;
  // 与上一音频块是否连续。恢复/断开/取消后为 false；不连续块必须显式复位对齐。
  bool continuous = true;
  // 不连续时的诊断原因；音频块为空。
  std::string reason;
};

using AlsaPlaybackReferenceCallback =
    std::function<void(const AlsaPlaybackReferenceBlock&)>;

// 适配器只读计数。所有字段在 open() 开始时重置，close() 后保留最后一次轮次快照。
struct AlsaAudioSinkStats {
  std::uint64_t open_attempts = 0;
  std::uint64_t open_successes = 0;
  std::uint64_t close_calls = 0;
  std::uint64_t write_calls = 0;
  std::uint64_t frames_consumed = 0;
  std::uint64_t backend_writes = 0;
  std::uint64_t short_writes = 0;
  std::uint64_t underruns = 0;
  std::uint64_t device_failures = 0;
  std::uint64_t reconnect_attempts = 0;
  std::uint64_t reconnect_successes = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t cancels = 0;
  std::uint64_t reference_blocks = 0;
  std::uint64_t reference_discontinuities = 0;
  std::uint64_t drained = 0;
  std::uint32_t actual_sample_rate_hz = 0;
  std::uint16_t actual_channels = 0;
  PlaybackSampleFormat actual_format = PlaybackSampleFormat::kS16LE;
  std::size_t actual_period_frames = 0;
  std::size_t actual_buffer_frames = 0;
};

// 校验适配器策略配置：写超时、后端写次数、重连/恢复预算和排空超时必须有界且有效。
domain::OperationResult validate_alsa_audio_sink_config(
    const AlsaAudioSinkConfig& config);

// 真实 ALSA 输出适配器。构造接收后端所有权；析构先 close() 后端并清空转换缓存。
// 同一实例由调用方串行调用 open/write/close；cancel 允许与 write 并发。
class AlsaAudioSink final : public capability::IAudioSink {
 public:
  AlsaAudioSink(AlsaAudioSinkConfig config,
                std::unique_ptr<PcmPlaybackBackend> backend);
  ~AlsaAudioSink() override;

  AlsaAudioSink(const AlsaAudioSink&) = delete;
  AlsaAudioSink& operator=(const AlsaAudioSink&) = delete;

  // 建立播放轮次：校验配置与后端所有权，调用 backend->open()，成功后清空转换缓存
  // 与参考序号。首次成功；已打开未 close 返回 kAlreadyCompleted；后端为空或配置
  // 非法返回 kInvalidInput；打开失败保持未打开，允许修正环境后重试。
  domain::OperationResult open() override;

  // 写入一帧统一 16 kHz/单声道/S16_LE/320 样本。成功表示整帧已转换为原生格式并
  // 被设备接受；未打开/关闭返回 kDeviceFailure；取消返回 kCancelled；预算内未完成
  // 返回 kTimeout；设备断开且恢复失败返回 kDeviceFailure。失败时可能已有部分原生
  // 样本被设备接受，调用方不得盲目重放同一帧。
  domain::OperationResult write(const domain::AudioFrame& frame) override;

  // 取消当前轮次：丢弃适配器转换缓存与设备尚未播放的可丢弃数据，唤醒阻塞 write。
  // 幂等、不抛异常；已物理播放的声音不撤回。close/open 后才能开始下一轮。
  domain::OperationResult cancel() noexcept override;

  // 正常关闭：先尝试在 drain_timeout 内排空设备缓冲，再关闭句柄；取消后的 close
  // 直接丢弃剩余数据。幂等、不抛异常。
  domain::OperationResult close() noexcept override;

  // 打开成功后的实际设备格式；未打开返回无效默认值。
  PcmPlaybackFormat actual_playback_format() const noexcept;

  // 设置实际写入设备的回放参考回调。回调在 write 的调用线程上同步执行；必须快速
  // 返回、不得重入本对象、不得抛异常。传空函数对象取消回调。回调生命周期由调用方
  // 保证；本类持有其副本，但 set 与正在进行的 write 并发由调用方避免。
  void set_playback_reference_callback(AlsaPlaybackReferenceCallback callback) noexcept;

  // 适配器计数快照；运行结束后读取，不改变状态。
  AlsaAudioSinkStats stats() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::backend

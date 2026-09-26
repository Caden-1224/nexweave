// 全双工音频前端：一个拥有者统一协调双向设备、实际播放参考和 AEC/降噪。
//
// 职责
// ----
// DuplexAudioFrontend 独占一个采集后端与一个播放后端，内部创建并拥有
// AlsaAudioSource/AlsaAudioSink，向上分别暴露一个 IAudioSource（处理后的
// 16 kHz/单声道/S16_LE/320 样本帧）和一个 IAudioSink（由 Session/TTS 写出
// 20 ms 领域帧）。前端拥有 AEC 所需的全部中间状态：20 ms 帧拆成两个 10 ms
// 窗口、实际成功写入设备的播放参考、参考重采样与时间对齐、设备恢复和状态
// 观测。Session/ASR 只通过已有 capability 契约使用它，不能持有 snd_pcm_t、
// WebRTC 句柄或播放参考队列。
//
// 两条生命周期
// ------------
// 采集生命周期与回答生命周期分离：
//   - FrontendAudioSource::open()/close() 控制常驻采集，回答结束、播放停止或
//     Session 取消都不得调用 close()；
//   - FrontendAudioSink::open()/close() 控制一次或一组播放轮次，停止回答只会
//     停止播放并丢弃未写出的数据，不会关闭常驻采集；
//   - 两者可以按任意顺序打开，但只有源打开时才会处理近端帧；只有播放打开时
//     才会收到实际写入的设备参考。
//
// AEC 对齐与不连续
// ----------------
// 播放参考只在 AlsaAudioSink 成功接受原生样本后产生，包含序列号、实际格式、
// 写入时间、设备剩余延迟帧数和连续标志。前端把这些样本转换为 16 kHz 单声道，
// 并按“预计实际播放时间”排队；采集线程在每个 10 ms 近端槽消费参考，未到播放
// 时间的槽填零。短写、underrun、取消、设备重连或关闭都会触发
// AlsaAudioSink 的不连续事件，前端清空参考队列、重置转换器和 AEC 状态后再从
// 新时间轴重新收敛。队列溢出或参考迟到超过配置上界也被视为显式不连续。
//
// 状态与设备恢复
// --------------
// capture_state()/playback_state() 只描述设备资源；aec_state() 独立描述前处理
// 收敛状态。设备打开成功绝不等于 AEC 已收敛。采集读失败后前端在
// reconnect_retry_interval/reconnect_total_budget 预算内关闭失效句柄、重置
// 转换与 AEC 状态并重新打开；预算耗尽后采集进入 kFailed 并保留结构化错误。
// 播放侧失败由 AlsaAudioSink 内部的有限恢复预算处理，前端原样上报。
//
// 资源与线程
// ----------
// 构造时接管两个 Pcm*Backend 与 IAudioProcessor 的所有权，但不打开设备、不创建
// 线程。FrontendAudioSource::open() 打开采集、打开前处理器并启动唯一的采集线程；
// close() 取消阻塞读、join 线程、关闭设备并释放前处理器。FrontendAudioSink 的
// open/write/cancel/close 由调用方串行调用，cancel 允许与 write 并发；它不创建
// 播放线程。析构会先停止采集线程和播放资源，再释放所有句柄。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "../backend/alsa/alsa_audio_sink.hpp"
#include "../backend/alsa/alsa_audio_source.hpp"
#include "../capability/backend.hpp"
#include "audio_processor.hpp"

namespace nexweave::runtime {

class DuplexAudioFrontend;

// 采集设备/播放设备的状态。kReady 只表示设备句柄和基础格式可用；
// kRecovering 表示失败后正在有限预算内重试；kFailed 表示预算耗尽或不可恢复。
enum class AudioPipelineState : std::uint8_t {
  kStopped,
  kStarting,
  kReady,
  kRecovering,
  kFailed,
};

// 全双工前端配置。设备原生格式在 Pcm*BackendConfig 中；这里的字段只约束拥有者
// 的队列、参考时间和恢复策略，避免把设备参数与编排策略混成一个含义模糊结构。
struct AudioFrontendConfig {
  // 采集适配器策略；真实设备参数已经固化在注入的 PcmCaptureBackend 中。
  backend::AlsaAudioSourceConfig source{};
  // 播放适配器策略；真实设备参数已经固化在注入的 PcmPlaybackBackend 中。
  backend::AlsaAudioSinkConfig sink{};
  // 已处理 20 ms 帧的等待队列上限。达到上限时丢弃最旧帧并计数，避免采集线程
  // 因为消费者停顿而无限阻塞；丢弃事实由 stats() 的 dropped_processed_frames
  // 单独可见，不能当作未发生。
  std::size_t processed_frame_queue_capacity = 64;
  // 16 kHz 渲染参考队列的样本上限。参考写入远快于实时播放时队列可能暂时增长；
  // 超过该上限表示时间轴已经不可靠，前端会清空队列并记一次显式不连续。
  std::size_t render_reference_sample_capacity = 32000;
  // 参考样本可以提前多少个 10 ms 窗口进入 AEC；默认半个窗口，用于吸收调度
  // 抖动而不把未来音频提前送给 AEC。
  std::chrono::milliseconds reference_schedule_tolerance{5};
  // 参考样本迟到超过该值即视为时间轴不可靠，并在下一个窗口重置 AEC。
  std::chrono::milliseconds max_reference_lateness{50};
  // 采集设备失败后每次重开之间的等待时间；必须大于 0。
  std::chrono::milliseconds reconnect_retry_interval{200};
  // 采集设备失败后重开的总预算；到期仍失败则进入 kFailed。
  std::chrono::milliseconds reconnect_total_budget{3000};
};

// 前端只读计数。capture/playback 字段描述设备生命周期；reference 字段描述
// 实际播放参考时间轴；processor 字段描述 AEC/降噪内部状态。所有字段都是快照。
struct AudioFrontendStats {
  AudioPipelineState capture_state = AudioPipelineState::kStopped;
  AudioPipelineState playback_state = AudioPipelineState::kStopped;
  AudioProcessorState aec_state = AudioProcessorState::kClosed;
  std::uint64_t capture_open_attempts = 0;
  std::uint64_t capture_open_successes = 0;
  std::uint64_t capture_close_calls = 0;
  std::uint64_t capture_reads = 0;
  std::uint64_t capture_frames = 0;
  std::uint64_t capture_dropped_frames = 0;
  std::uint64_t capture_reconnect_attempts = 0;
  std::uint64_t capture_reconnect_successes = 0;
  std::uint64_t capture_reconnect_failures = 0;
  std::uint64_t playback_open_attempts = 0;
  std::uint64_t playback_open_successes = 0;
  std::uint64_t playback_close_calls = 0;
  std::uint64_t playback_writes = 0;
  std::uint64_t playback_frames = 0;
  std::uint64_t render_reference_blocks = 0;
  std::uint64_t render_reference_samples_queued = 0;
  std::uint64_t render_reference_samples_consumed = 0;
  std::uint64_t render_reference_zero_filled = 0;
  std::uint64_t render_reference_late_samples = 0;
  std::uint64_t render_reference_discontinuities = 0;
  std::uint64_t render_reference_overflows = 0;
  std::uint64_t processor_resets = 0;
  std::uint64_t processor_failures = 0;
  backend::PcmCaptureFormat actual_capture_format{};
  backend::PcmPlaybackFormat actual_playback_format{};
  AudioProcessorStats processor{};
};

// 处理后音频源适配器：把前端的处理队列暴露为统一 IAudioSource。
//
// 轮次语义：open() 建立常驻采集轮次并启动处理线程；read() 返回自有的
// 16 kHz/单声道/S16_LE/320 样本帧；cancel() 取消当前采集轮次并丢弃未消费队列，
// 之后必须 close/open 才能开始下一轮；close() 停止线程、关闭采集设备并释放
// 前处理器。回答收尾不得调用 close()，否则会破坏常驻采集不变量。
class FrontendAudioSource final : public capability::IAudioSource {
 public:
  explicit FrontendAudioSource(DuplexAudioFrontend& owner) noexcept;
  ~FrontendAudioSource() override;

  FrontendAudioSource(const FrontendAudioSource&) = delete;
  FrontendAudioSource& operator=(const FrontendAudioSource&) = delete;

  domain::OperationResult open() override;
  domain::Result<domain::AudioFrame> read() override;
  domain::OperationResult cancel() noexcept override;
  domain::OperationResult close() noexcept override;

 private:
  DuplexAudioFrontend& owner_;
};

// 播放汇适配器：把 20 ms 领域帧交给内部 AlsaAudioSink，同时让前端的参考回调
// 捕获实际成功写入设备的样本。write() 的返回语义沿用 IAudioSink：成功表示帧
// 已被设备接受，而不是已经播完；cancel() 丢弃尚未播放的可丢弃数据，不能撤回
// 已经物理播放的声音。
class FrontendAudioSink final : public capability::IAudioSink {
 public:
  explicit FrontendAudioSink(DuplexAudioFrontend& owner) noexcept;
  ~FrontendAudioSink() override;

  FrontendAudioSink(const FrontendAudioSink&) = delete;
  FrontendAudioSink& operator=(const FrontendAudioSink&) = delete;

  domain::OperationResult open() override;
  domain::OperationResult write(const domain::AudioFrame& frame) override;
  domain::OperationResult cancel() noexcept override;
  domain::OperationResult close() noexcept override;

 private:
  DuplexAudioFrontend& owner_;
};

// 全双工音频前端拥有者。source()/sink() 返回内部适配器的稳定引用，二者必须与
// 本对象同寿命；调用方不得在适配器之外再打开同一对设备。
class DuplexAudioFrontend {
 public:
  // 接管三个对象的所有权。capture_backend 与 playback_backend 可以是真实
  // ALSA 后端或受控 Fake；processor 可以是真实 WebRTC 处理器或受控 Fake。
  // 构造不打开设备、不创建线程；配置非法时不构造半启动状态（由调用方先校验
  // 或由构造后返回状态可观察的失败）。
  DuplexAudioFrontend(AudioFrontendConfig config,
                      std::unique_ptr<backend::PcmCaptureBackend> capture_backend,
                      std::unique_ptr<backend::PcmPlaybackBackend> playback_backend,
                      std::unique_ptr<IAudioProcessor> processor);
  ~DuplexAudioFrontend();

  DuplexAudioFrontend(const DuplexAudioFrontend&) = delete;
  DuplexAudioFrontend& operator=(const DuplexAudioFrontend&) = delete;

  // 稳定适配器引用；只能在对象存活期间使用。
  FrontendAudioSource& source() noexcept;
  FrontendAudioSink& sink() noexcept;

  // 状态与错误快照。只读、不阻塞；实现内部用互斥量保证快照一致。
  AudioPipelineState capture_state() const;
  AudioPipelineState playback_state() const;
  AudioProcessorState aec_state() const;
  domain::Error capture_error() const;
  domain::Error playback_error() const;
  domain::Error processor_error() const;

  // 真实协商格式；对应生命周期未打开时返回默认无效值。
  backend::PcmCaptureFormat actual_capture_format() const noexcept;
  backend::PcmPlaybackFormat actual_playback_format() const noexcept;

  // 全量只读计数。可在运行中轮询，不改变状态。
  AudioFrontendStats stats() const;

  // 以下入口由 FrontendAudioSource/FrontendAudioSink 调用；也允许测试直接调用
  // 来验证设备生命周期与取消语义。它们都幂等或有明确状态错误，且不抛出异常。
  domain::OperationResult OpenCapture();
  domain::Result<domain::AudioFrame> ReadProcessed();
  domain::OperationResult CancelCapture() noexcept;
  domain::OperationResult CloseCapture() noexcept;
  domain::OperationResult OpenPlayback();
  domain::OperationResult WritePlayback(const domain::AudioFrame& frame);
  domain::OperationResult CancelPlayback() noexcept;
  domain::OperationResult ClosePlayback() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<FrontendAudioSource> source_;
  std::unique_ptr<FrontendAudioSink> sink_;
};

}  // namespace nexweave::runtime

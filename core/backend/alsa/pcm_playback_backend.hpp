// NexWeave PCM 播放后端接缝：把真实 ALSA 播放设备与上层音频汇隔离。
//
// 职责与适用范围
// --------------
// 本文件只定义“连续播放设备如何打开、写入、排空、恢复、取消和关闭”的内部接缝，
// 不定义 Session 可见的 capability::IAudioSink，也不暴露 snd_pcm_t 或 ALSA 类型。
// `AlsaAudioSink` 是唯一产品调用方；测试用受控后端验证短写、underrun、超时、取消、
// 断开、重连、排空和回放参考路径。
//
// 原生样本表示
// ------------
// 播放后端的样本统一使用 int32_t 存储：
//   - kS16LE：每个样本是有符号 16 位值，范围 [-32768, 32767]；
//   - kS24LE：每个样本是有符号 24 位值，范围 [-8388608, 8388607]。
// 多声道样本按帧交错；帧数 = 样本数 / channels。适配器负责把 16 kHz 单声道
// S16_LE 领域帧显式重采样、复制声道并转换为后端声明的原生格式。
//
// 所有权与并发
// ------------
// 后端对象由 AlsaAudioSink 独占拥有，析构者负责最终关闭句柄。open/write/
// recover/drain/close 由适配器设备互斥量串行化；cancel 允许与 write 并发，必须
// 通过原子标志或等价通知在有限等待内唤醒阻塞写。后端不创建后台线程、不回调上层。
//
// 播放语义
// --------
// write 只表示设备“接受了多少帧”，不表示这些帧已经播完；drain 才表示设备缓冲被
// 排空。cancel 丢弃设备中尚未播放的可丢弃数据，不能撤回已经物理播放的声音。
// 失败和不连续必须显式返回/标记，不能用“写入返回成功”冒充“已经静默”。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../../domain/error.hpp"

namespace nexweave::backend {

// 播放设备原生样本编码。与采集后端分开命名，避免把输入缓冲语的容器宽度
// 语义误用到输出；当前支持板端实测的 S16_LE/S24_LE。
enum class PlaybackSampleFormat : std::uint8_t {
  kS16LE,
  kS24LE,
};

// 播放后端打开参数。所有字段必须显式配置并在 open 前校验，设备名不进入核心领域。
struct PcmPlaybackBackendConfig {
  // ALSA PCM 设备名，例如 hw:0,0、plughw:0,0 或 default；空字符串非法。
  std::string device;
  // 请求的原生播放采样率，单位 Hz；允许后端通过 rate_near 返回邻近实际值，
  // 适配器会按实际值显式重采样。
  std::uint32_t sample_rate_hz = 16000;
  // 请求的原生声道数；板端支持 1～2。
  std::uint16_t channels = 1;
  // 请求的原生样本编码。
  PlaybackSampleFormat format = PlaybackSampleFormat::kS16LE;
  // 单次写入的目标 period 帧数；必须大于 0。
  std::size_t period_frames = 320;
  // 设备缓冲帧数请求值，必须不小于 period_frames。
  std::size_t buffer_frames = 1280;
  // 设备暂时不可写时每次 poll/wait 的最长时间；cancel 的有限唤醒以此为上界之一。
  std::size_t write_poll_slice_ms = 20;
  // 正常关闭时检查“设备缓冲是否已播完”的轮询间隔；用于把 drain 变成有限等待。
  std::size_t drain_poll_slice_ms = 10;
};

// open 成功后回报的实际格式。采样率/声道数可能因硬件协商与请求值不同。
struct PcmPlaybackFormat {
  std::uint32_t sample_rate_hz = 0;
  std::uint16_t channels = 0;
  PlaybackSampleFormat format = PlaybackSampleFormat::kS16LE;

  bool valid() const noexcept {
    return sample_rate_hz > 0 && (channels == 1 || channels == 2);
  }
};

// 一次 write 的结果类型。kWritten 表示 frames_written 帧已被设备接受；
// kRecovered 表示检测到 underrun/挂起并已恢复低层状态，本次可能没有或只有部分
// 数据被接受；调用方需把 recovered 作为“参考不连续”边界。
enum class PcmPlaybackWriteKind : std::uint8_t {
  kWritten,          // frames_written 帧已被设备接受
  kRecovered,        // 发生 underrun/挂起并已恢复低层状态
  kTimeout,          // 本次等待窗口内设备没有可写空间
  kCancelled,        // 后端已进入取消状态，本次 write 不写出数据
  kDeviceFailure,    // 设备断开或不可恢复的写入错误
  kAlreadyCompleted  // 后端已关闭或不再接受写入
};

// write 返回值。frames_written 是本次实际被设备接受的帧数；0 也可能是正常结果。
// delay_frames 是写入成功后设备仍待播放的估计帧数，-1 表示后端无法查询。
struct PcmPlaybackWriteResult {
  PcmPlaybackWriteKind kind = PcmPlaybackWriteKind::kDeviceFailure;
  std::size_t frames_written = 0;
  std::int64_t delay_frames = -1;
  domain::Error error{};

  static PcmPlaybackWriteResult Written(std::size_t frames, std::int64_t delay = -1) {
    PcmPlaybackWriteResult result;
    result.kind = PcmPlaybackWriteKind::kWritten;
    result.frames_written = frames;
    result.delay_frames = delay;
    return result;
  }

  static PcmPlaybackWriteResult Status(PcmPlaybackWriteKind kind,
                                       domain::ErrorCode code = domain::ErrorCode::kNone,
                                       std::string message = {}) {
    PcmPlaybackWriteResult result;
    result.kind = kind;
    result.error.code = code;
    result.error.message = std::move(message);
    return result;
  }
};

// 播放后端统计。计数属于后端对象实例，open/close 不重置，便于观察恢复和资源归属。
struct PcmPlaybackBackendStats {
  std::uint64_t open_attempts = 0;
  std::uint64_t open_successes = 0;
  std::uint64_t close_calls = 0;
  std::uint64_t write_calls = 0;
  std::uint64_t written_calls = 0;
  std::uint64_t recovered_calls = 0;
  std::uint64_t timeout_calls = 0;
  std::uint64_t cancelled_calls = 0;
  std::uint64_t device_failures = 0;
  std::uint64_t drain_calls = 0;
  std::uint64_t drain_timeouts = 0;
  std::uint64_t recover_attempts = 0;
  std::uint64_t recover_successes = 0;
  std::uint64_t short_writes = 0;
  std::uint32_t actual_sample_rate_hz = 0;
  std::uint16_t actual_channels = 0;
  PlaybackSampleFormat actual_format = PlaybackSampleFormat::kS16LE;
  std::size_t actual_period_frames = 0;
  std::size_t actual_buffer_frames = 0;
};

// PCM 播放后端接缝。
class PcmPlaybackBackend {
 public:
  virtual ~PcmPlaybackBackend() = default;

  // 打开设备并完成显式格式协商；重复调用且未 close 返回 kAlreadyCompleted。
  virtual domain::OperationResult open() = 0;

  // 写入最多 frames 帧原生交错样本。interleaved_samples 必须至少有
  // frames*current_format().channels 个元素，并在调用期间保持有效。返回实际接受
  // 帧数；短写、underrun、取消、设备失败和超时通过 kind/error 表达。
  virtual PcmPlaybackWriteResult write(const std::int32_t* interleaved_samples,
                                       std::size_t frames,
                                       std::chrono::milliseconds timeout) = 0;

  // 设备发生不可恢复写入错误后尝试重新打开一次。cancel 可与它并发；成功后
  // current_format() 表示新设备状态，适配器必须把回放参考标记为不连续。
  virtual domain::OperationResult recover() = 0;

  // 排空设备中尚未播放的数据；timeout 内未排空返回 kTimeout。只读等待，不写新数据。
  virtual domain::OperationResult drain(std::chrono::milliseconds timeout) = 0;

  // 取消当前写入轮次并丢弃设备中尚未播放的可丢弃数据；不撤回已物理播放的声音。
  // 幂等、不抛异常，必须有限唤醒阻塞 write。
  virtual domain::OperationResult cancel() noexcept = 0;

  // 关闭设备并释放句柄；幂等，返回后 current_format() 无效。
  virtual domain::OperationResult close() noexcept = 0;

  // 打开成功后的设备实际格式；未打开时返回无效默认值。
  virtual PcmPlaybackFormat current_format() const noexcept = 0;

  // 返回计数和实际协商参数快照；不修改设备状态。
  virtual PcmPlaybackBackendStats stats() const noexcept = 0;
};

// 校验播放后端配置：设备非空、采样率/声道/period/buffer/poll 合法且互相一致。
domain::OperationResult validate_pcm_playback_backend_config(
    const PcmPlaybackBackendConfig& config);

}  // namespace nexweave::backend

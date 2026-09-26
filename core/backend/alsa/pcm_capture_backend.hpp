// NexWeave PCM 采集后端接缝：把真实 ALSA 设备与上层输入适配器隔离。
//
// 职责与适用范围
// --------------
// 本文件只定义“连续采集设备如何打开、读取、恢复、取消和关闭”的内部接缝，不定义
// Session 可见的能力接口，也不暴露 snd_pcm_t 或任何厂商类型。`AlsaAudioSource` 是
// 该接缝的唯一产品调用方；测试可以提供受控实现，在不接触声卡的情况下验证阻塞、超时、
// 短读、溢出、断开和重连路径。
//
// 数据表示与单位
// --------------
// 一个 `PcmCaptureBlock` 保存设备原生格式的一批交错样本。样本统一存放在 int32_t 中：
//   - kS16LE：每个样本按 16 位有符号整数解释后扩展为 int32_t，范围 [-32768, 32767]；
//   - kS24LE：每个样本按要求放进 int32_t 的低 24 位，范围 [-8388608, 8388607]。
// 帧数等于 `interleaved_samples.size() / channels`；适配器负责显式转单声道、S16_LE、
// 16 kHz 和 320 样本帧，后端不得通过改帧头假装已经完成转换。
//
// 所有权与并发
// ------------
// 后端对象由 `AlsaAudioSource` 独占拥有，析构者负责最终关闭设备。`open`、`read`、
// `recover`、`close` 由适配器用设备互斥量串行化；`cancel` 允许与 `read` 并发，必须
// 使用原子标志或无锁通知在有限等待内唤醒阻塞读。后端不得在回调中调用上层。
//
// 失败与诊断
// ----------
// 所有错误映射到 domain::ErrorCode；`read` 用 `PcmCaptureReadKind` 区分数据、超时、
// 已恢复、取消、设备失败和不可再读。错误信息只用于诊断，不携带口令、设备密钥或完整
// 环境变量。后端返回 kRecovered 时表示本次没有数据但内部状态已经恢复，调用方应在
// 预算内重试，不能把它当成空帧静默丢弃。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../../domain/error.hpp"

namespace nexweave::backend {

// 设备原生样本编码。只支持板端实测覆盖的两种小端有符号 PCM；后续新增编码必须
// 作为显式枚举值扩展，不能复用已有数值改变语义。
enum class PcmSampleFormat : std::uint8_t {
  kS16LE,
  kS24LE,
};

// 真实设备或测试后端的打开参数。所有字段都必须显式配置，构造后由校验函数检查；
// 设备名不进入核心领域契约，只在本适配器边界内使用。
struct PcmCaptureBackendConfig {
  // ALSA PCM 设备名，例如 hw:0,0、plughw:0,0 或 default；空字符串非法。
  std::string device;
  // 向设备请求的原生采样率，单位 Hz。允许设备返回 rate_near 后的邻近值，
  // 适配器会按实际值显式重采样，不把 16 kHz 写死在设备打开参数里。
  std::uint32_t sample_rate_hz = 16000;
  // 向设备请求的原生声道数。板端范围是 1～2；非法值在打开前拒绝。
  std::uint16_t channels = 1;
  // 向设备请求的原生样本格式。
  PcmSampleFormat format = PcmSampleFormat::kS16LE;
  // 单次 read 期望的最大帧数，也是 ALSA period 的请求值；必须大于 0。
  std::size_t period_frames = 320;
  // 设备缓冲帧数请求值，必须不小于 period_frames；真值由 ALSA 协商后回报。
  std::size_t buffer_frames = 1280;
  // 阻塞等待数据时每次进入 poll/wait 的最长时间。它同时给取消提供有限唤醒上界：
  // cancel 后后端最多再等待一个 slice 就会重新检查取消标志。必须大于 0。
  std::size_t read_poll_slice_ms = 20;
};

// 打开成功后回报的实际格式。采样率/声道数可能因设备协商与请求值不同；格式是
// 后端实际配置成功的编码，供适配器选择正确转换路径。
struct PcmCaptureFormat {
  std::uint32_t sample_rate_hz = 0;
  std::uint16_t channels = 0;
  PcmSampleFormat format = PcmSampleFormat::kS16LE;

  // 只有三个字段都处于可转换范围时返回 true。该检查不访问设备，不分配。
  bool valid() const noexcept {
    return sample_rate_hz > 0 && (channels == 1 || channels == 2);
  }
};

// 一批原生交错样本。对象拥有样本存储；移动后源对象不再可用。
struct PcmCaptureBlock {
  PcmCaptureFormat format;
  std::vector<std::int32_t> interleaved_samples;

  // 帧数；channels 为 0 或样本数不是整数帧时返回 0，调用方按无效块处理。
  std::size_t frame_count() const noexcept {
    if (format.channels == 0) {
      return 0;
    }
    if (interleaved_samples.size() % format.channels != 0) {
      return 0;
    }
    return interleaved_samples.size() / format.channels;
  }
};

// 一次 read 的结果类型。使用显式枚举而不是仅靠 ErrorCode，是为了让“恢复后重试”
// 与“设备失败”在类型上分开，避免调用方把空数据当成功或把恢复当故障。
enum class PcmCaptureReadKind : std::uint8_t {
  kData,             // block 有效，包含至少一帧数据
  kTimeout,          // 本次等待窗口内无数据，设备状态未知但对象仍可重试
  kRecovered,        // 检测到 overrun/underrun 并已完成低层恢复，本次无数据
  kCancelled,        // 后端已进入取消状态，本次 read 不返回数据
  kDeviceFailure,    // 设备断开、参数失效或不可恢复的读取错误
  kAlreadyCompleted  // 输入已自然结束或后端已关闭，不再产生数据
};

// read 的返回值。kind==kData 时 block 有效；其余情况 block 为空。error 只在
// error.code!=kNone 时用于结构化诊断，超时也允许携带可读说明。
struct PcmCaptureReadResult {
  PcmCaptureReadKind kind = PcmCaptureReadKind::kDeviceFailure;
  PcmCaptureBlock block;
  domain::Error error{};

  // 构造数据结果；block 为空时仍返回 kData，但调用方应在适配器中校验帧数。
  static PcmCaptureReadResult Data(PcmCaptureBlock block) {
    PcmCaptureReadResult result;
    result.kind = PcmCaptureReadKind::kData;
    result.block = std::move(block);
    return result;
  }

  // 构造带错误码和诊断信息的状态结果。
  static PcmCaptureReadResult Status(PcmCaptureReadKind kind,
                                     domain::ErrorCode code = domain::ErrorCode::kNone,
                                     std::string message = {}) {
    PcmCaptureReadResult result;
    result.kind = kind;
    result.error.code = code;
    result.error.message = std::move(message);
    return result;
  }
};

// 后端可观测计数。计数属于后端对象实例，open/close 不重置，便于验证恢复和资源归属。
struct PcmCaptureBackendStats {
  std::uint64_t open_attempts = 0;
  std::uint64_t open_successes = 0;
  std::uint64_t close_calls = 0;
  std::uint64_t read_calls = 0;
  std::uint64_t data_blocks = 0;
  std::uint64_t timeout_blocks = 0;
  std::uint64_t recovered_blocks = 0;
  std::uint64_t device_failures = 0;
  std::uint64_t cancel_calls = 0;
  std::uint64_t recover_attempts = 0;
  std::uint64_t recover_successes = 0;
  std::uint64_t short_reads = 0;
  std::uint32_t actual_sample_rate_hz = 0;
  std::uint16_t actual_channels = 0;
  PcmSampleFormat actual_format = PcmSampleFormat::kS16LE;
  std::size_t actual_period_frames = 0;
  std::size_t actual_buffer_frames = 0;
};

// PCM 采集后端接缝。所有方法都必须满足上述并发约定；实现可以在 read 中阻塞，
// 但最长等待由调用方传入的 timeout 控制，cancel 必须能在有限等待内让它退出。
class PcmCaptureBackend {
 public:
  virtual ~PcmCaptureBackend() = default;

  // 打开设备并完成格式协商。成功后才能 read；重复调用且尚未 close 时返回
  // kAlreadyCompleted。打开失败不得留下半初始化句柄。
  virtual domain::OperationResult open() = 0;

  // 在 timeout 内等待并读取最多 period_frames 帧。返回数据、超时、已恢复、
  // 取消、设备失败或不可再读；短读通过小于 period_frames 的数据块表达。
  virtual PcmCaptureReadResult read(std::chrono::milliseconds timeout) = 0;

  // 设备发生不可恢复读取错误后尝试重新打开一次。要求：
  //   - 只有同一个对象的 read 线程调用；
  //   - cancel 可以与它并发；若恢复期间收到取消，应在完成后返回 kCancelled，
  //     且不得把已打开的设备句柄泄漏出去；
  //   - 返回成功后 current_format() 必须是新设备配置，调用方必须丢弃旧转换缓存。
  virtual domain::OperationResult recover() = 0;

  // 置取消标志并唤醒阻塞 read。幂等、不抛异常；不释放句柄，close 仍负责收尾。
  virtual domain::OperationResult cancel() noexcept = 0;

  // 关闭设备并释放句柄。幂等；返回后 current_format() 无效，后续 read 返回失败。
  virtual domain::OperationResult close() noexcept = 0;

  // 打开成功后返回设备实际格式；未打开时返回默认无效值。只读快照，允许从任意线程调用。
  virtual PcmCaptureFormat current_format() const noexcept = 0;

  // 返回计数和实际协商参数快照；不得修改设备状态。
  virtual PcmCaptureBackendStats stats() const noexcept = 0;
};

// 校验后端配置：设备非空、采样率/声道/period/buffer/poll slice 合法且互相一致。
// 只读、不分配外部资源；失败返回 kInvalidInput，成功返回 kNone。
domain::OperationResult validate_pcm_capture_backend_config(
    const PcmCaptureBackendConfig& config);

}  // namespace nexweave::backend

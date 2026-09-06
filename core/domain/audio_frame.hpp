// NexWeave 统一音频帧领域契约。
//
// 职责：为 ASR、VAD、TTS 和数据面提供同一组 PCM 帧元数据与固定长度样本。
// 本契约只表达 16 kHz、单声道、S16_LE、20 ms 的帧，不持有设备句柄、文件、
// socket 或线程；样本由值对象拥有，调用方可在栈、容器或消息中安全传递。
// 输入前提：构造工厂接受任意样本数量，但只有恰好 320 个 int16_t 样本可成功；
// 工厂复制输入，调用方仍拥有原始容器。复制可能分配内存，分配失败按标准库异常
// 传播，且临时对象会自动清理，不会留下半构造帧。
// 输出后置：成功结果包含完整且可校验的 AudioFrame；失败结果保留默认帧并给出
// 结构化错误码，绝不截断、补零或静默重采样。校验函数无阻塞，但成功时复制样本，
// 可能因分配失败抛出标准库异常；工厂同样可能抛出分配异常。只读调用可并发执行；同一个 AudioFrame
// 实例不提供内部同步，跨线程修改须由
// 调用方同步。固定元数据是 v1 合同，后续版本只能通过显式新契约扩展，不能改变
// 既有常量的含义。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nexweave::domain {

inline constexpr std::uint32_t kAudioSampleRateHz = 16000;
inline constexpr std::uint16_t kAudioChannels = 1;
inline constexpr std::uint32_t kAudioFrameDurationMs = 20;
inline constexpr std::size_t kAudioFrameSamples = 320;
inline constexpr std::size_t kAudioFrameBytes = kAudioFrameSamples * sizeof(std::int16_t);

// 保留未知值以便验证来自协议或设备的反序列化字段；当前唯一允许值是 kS16LE。
enum class AudioSampleFormat : std::uint8_t {
  kS16LE,
  kUnknown,
};

enum class AudioFrameError : std::uint8_t {
  kNone,               // 元数据与样本数均符合 v1 合同。
  kInvalidSampleRate,  // 采样率不是 16000 Hz；调用方应拒绝该帧，不做隐式重采样。
  kInvalidChannels,  // 声道数不是单声道；交错多声道数据不能直接提交。
  kInvalidFormat,    // 样本编码不是 S16_LE；字节序/位宽不匹配时使用此错误。
  kInvalidSampleCount,  // 样本数不是 320；短帧、长帧和缺失载荷均属于此错误。
};

struct AudioFrameValidationResult;

// 固定格式的 20 ms PCM 帧。字段保持公开是为了让协议/适配器能够观察元数据；
// 修改后必须再次调用 validate_audio_frame，Session 不应接受未经校验的对象。
struct AudioFrame {
  std::uint32_t sample_rate_hz = kAudioSampleRateHz;
  std::uint16_t channels = kAudioChannels;
  AudioSampleFormat format = AudioSampleFormat::kS16LE;
  std::vector<std::int16_t> samples;

  // 从固定大小数组复制样本；N 非 320 时在编译期分支返回 kInvalidSampleCount，
  // 不截断输入。成功后结果拥有独立样本存储，源数组可立即释放；可能抛出分配异常。
  template <std::size_t N>
  static AudioFrameValidationResult from_samples(const std::array<std::int16_t, N>& input);

  // 从运行时容器复制样本；只有 size()==320 成功，适用于协议/设备解码后的动态载荷。
  // 输入容器归调用方所有，函数不保留其引用；长度错误返回 kInvalidSampleCount。
  static AudioFrameValidationResult from_samples(const std::vector<std::int16_t>& input);
};

// 工厂与校验共用的显式结果。error==kNone 时 value 满足全部音频不变量；错误时
// value 保持默认值，调用方必须检查 ok() 后才可提交到后端或数据面。
struct AudioFrameValidationResult {
  AudioFrame value;
  // 默认对象必须是不成功结果，避免“空载荷 + kNone”伪装成合法帧。
  AudioFrameError error = AudioFrameError::kInvalidSampleCount;

  // 返回 true 仅表示 value 已通过全部固定合同检查；false 时不得向下游传递 value。
  bool ok() const noexcept {
    return error == AudioFrameError::kNone;
  }
};

// 校验顺序固定为元数据后样本长度：错误码互斥且优先报告帧头错误，避免同一坏帧
// 因检查顺序改变而产生不同诊断。函数不修改输入帧；成功结果复制输入以便调用方
// 直接把校验结果传递给下游，失败时不复制不合约载荷。
AudioFrameValidationResult validate_audio_frame(const AudioFrame& frame);

template <std::size_t N>
AudioFrameValidationResult AudioFrame::from_samples(const std::array<std::int16_t, N>& input) {
  AudioFrameValidationResult result;
  if constexpr (N != kAudioFrameSamples) {
    result.error = AudioFrameError::kInvalidSampleCount;
    return result;
  } else {
    result.value.samples.assign(input.begin(), input.end());
    result.error = AudioFrameError::kNone;
    return result;
  }
}

}  // namespace nexweave::domain

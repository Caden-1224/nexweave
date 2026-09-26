// 音频前处理接缝：在固定 10 ms 领域窗口上执行回声消除与降噪。
//
// 职责与边界
// ----------
// 本接口只表达“一个实时音频前处理状态机如何接收渲染参考、处理近端帧并报告
// 收敛状态”。它不打开设备、不持有线程、不解析 20 ms 领域帧，也不向 Session
// 暴露 WebRTC 或 ALSA 类型。拥有者必须先按 20 ms 领域帧拆成两个 10 ms 帧，
// 再按 process_render() 后 process_capture() 的固定顺序调用。
//
// 固定窗口契约
// ------------
// 所有输入输出都必须是 160 个 int16_t 样本，对应 16 kHz、单声道、S16_LE 的
// 10 ms 窗口。实现不得接受 20 ms 或任意长度帧；拥有者也不得把 320 样本领域帧
// 直接传入。渲染参考与近端帧必须在同一时间轴上，且 process_render() 必须与
// 对应的 process_capture() 成对出现。
//
// 状态与收敛
// ----------
// open() 成功只表示前处理对象已初始化，不等于 AEC 已经收敛；实现必须在处理
// 过程中根据 delay 指标、回声抑制指标和已处理帧数把 state() 从
// kNotConverged 推进到 kConverging/kConverged。任何渲染参考不连续、设备重连、
// 取消或显式 reset() 都必须让实现回到 kNotConverged，并清空会跨帧影响结果的
// 算法状态；迟到样本不得拼接到旧时间轴。
//
// 线程与资源
// ----------
// 本接口不规定内部同步。调用方必须串行化 open/close/process_render/
// process_capture/reset；stats() 与 state() 可以由观察线程调用，但实现必须
// 保证读取到一致快照，或由具体实现说明其同步边界。open() 创建的外部句柄由
// close() 释放；析构前必须 close()。close() 必须幂等、不抛异常，失败路径不得
// 留下半初始化句柄。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "../domain/error.hpp"

namespace nexweave::runtime {

// 10 ms、16 kHz、单声道、S16_LE 的固定处理窗口。该常量是前处理接缝的一部分，
// 改变它等同于改变 AEC 算法接口，必须走显式的新契约而不是修改旧语义。
inline constexpr std::size_t kAudioProcessorFrameSamples = 160;
inline constexpr std::size_t kAudioProcessorFrameDurationMs = 10;
using AudioProcessorFrame = std::array<std::int16_t, kAudioProcessorFrameSamples>;

// 前处理状态机。设备就绪与 AEC 收敛是两种不同事实：本枚举只描述后者。
//   - kClosed：尚未 open 或已经 close，不能处理帧。
//   - kNotConverged：已初始化，但没有足够证据说明回声路径已经稳定。
//   - kConverging：已收到足够处理窗口，但收敛判据尚未全部满足。
//   - kConverged：配置的收敛判据在当前时间点成立；后续不连续会立即回到
//     kNotConverged。
//   - kFailed：初始化或处理已失败，必须显式 reset()/close()/open() 才能恢复。
enum class AudioProcessorState : std::uint8_t {
  kClosed,
  kNotConverged,
  kConverging,
  kConverged,
  kFailed,
};

// 前处理配置。所有阈值都是显式设计约束，不是已经实测出的硬件收敛值；板端实验
// 可以用同一配置读取原始指标后重新标定，但不得把默认阈值当成声学结论。
struct AudioProcessorConfig {
  // 从渲染参考进入处理器到对应回声出现在近端窗口之间的估计毫秒数，传给 AEC。
  // 必须非负；实际系统延迟由固定设备、缓冲和声程决定，未测量前不能声称准确。
  int stream_delay_ms = 40;
  // WebRTC 降噪等级：0 mild、1 medium、2 aggressive。越界时按 0～2 截断。
  int noise_suppression_level = 2;
  // 至少处理多少个渲染窗口后才允许进入收敛判定。500 个窗口等于 5 秒。
  std::size_t min_render_frames_for_convergence = 500;
  // 至少处理多少个近端窗口后才允许进入收敛判定。与渲染条件分开，避免只有
  // 播放没有采集时被误判为收敛。
  std::size_t min_capture_frames_for_convergence = 500;
  // 至少取得多少次延迟估计后才允许进入收敛判定。当前板端旧版 AEC 的
  // GetDelayMetrics 只交付一次聚合快照，因此该值取 1；调大它意味着实现必须
  // 能提供多次独立估计，否则状态会永远停在 kConverging。
  std::size_t min_delay_estimates_for_convergence = 1;
  // 延迟估计中“差延迟”比例上限；超过该值只能停留在 kConverging。
  float max_poor_delay_fraction = 0.10F;
  // 回声回损增强平均值下限，单位 dB。该阈值只作为收敛判据的一部分，必须与
  // delay 指标同时满足；没有实测校准时可以由调用方关闭。
  int min_erle_average_db = 3;
  // 是否要求 ERLE 下限。默认关闭，因为不同设备/距离下 ERLE 还未标定；
  // 任务 49 可在固定声学条件下开启并把结果写入证据。
  bool require_erle_for_convergence = false;
};

// 只读前处理快照。所有字段在 reset() 时保留累计计数，在当前收敛窗口上清零
// since_reset 字段；状态与指标用于区分“设备已打开”“正在收敛”“已收敛”和
// “处理失败”，不能只凭一个布尔值判断。
struct AudioProcessorStats {
  AudioProcessorState state = AudioProcessorState::kClosed;
  bool initialized = false;
  std::uint64_t render_frames = 0;
  std::uint64_t capture_frames = 0;
  std::uint64_t render_frames_since_reset = 0;
  std::uint64_t capture_frames_since_reset = 0;
  std::uint64_t resets = 0;
  std::uint64_t process_render_errors = 0;
  std::uint64_t process_capture_errors = 0;
  // 最近一次 reset 的原因，仅用于诊断；空串表示尚未 reset 过。
  std::string last_reset_reason;
  // 延迟与回声指标；-1 表示当前实现/当前时刻没有可用值。
  std::int32_t delay_median_ms = -1;
  std::int32_t delay_std_ms = -1;
  float fraction_poor_delays = -1.0F;
  std::int32_t erle_average_db = 0;
  std::int32_t erl_average_db = 0;
  std::int32_t a_nlp_average = 0;
  std::uint64_t delay_measurements = 0;
};

// 实时音频前处理能力。实现可以是真实软件算法，也可以是受控 Fake；拥有者只
// 依赖本接口，不需要知道具体算法库。
class IAudioProcessor {
 public:
  virtual ~IAudioProcessor() = default;

  // 初始化外部算法句柄。成功后将状态置为 kNotConverged，并允许处理 160 样本
  // 窗口。重复 open 且未 close 返回 kAlreadyCompleted；失败不得留下半初始化
  // 资源，调用方可以修正环境后重试。
  virtual domain::OperationResult open() = 0;

  // 提交一个渲染参考窗口。调用方保证帧来自实际成功写入设备的样本（或明确的
  // 静音替代），并已按时间轴对齐。该方法不产生输出；失败返回结构化错误。
  virtual domain::OperationResult process_render(const AudioProcessorFrame& frame) = 0;

  // 提交一个近端窗口并返回处理后窗口。调用方必须先对同一时间槽调用
  // process_render()。输入输出都是固定 160 样本；失败时不保证输出可用。
  virtual domain::Result<AudioProcessorFrame> process_capture(
      const AudioProcessorFrame& frame) = 0;

  // 清空跨帧算法状态并回到 kNotConverged。reason 只用于诊断，不参与机器判定。
  // 该入口用于渲染参考不连续、设备重连、取消和显式恢复；必须不抛异常或有
  // 明确错误返回。实现应保留累计错误与 reset 次数，便于证据核对。
  virtual domain::OperationResult reset(const std::string& reason) = 0;

  // 释放算法句柄并使对象回到 kClosed。幂等、不抛异常；重复调用成功。
  virtual domain::OperationResult close() noexcept = 0;

  // 返回状态快照。实现必须保证该调用不会与 open/close/process 并发时读到
  // 半更新状态；如果做不到，应由拥有者串行化后再调用。
  virtual AudioProcessorState state() const = 0;

  // 返回指标快照，语义同 state()。
  virtual AudioProcessorStats stats() const = 0;
};

}  // namespace nexweave::runtime

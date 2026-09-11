// 交互契约的无线程、确定性夹具。它把常驻输入流、语音段和回答 generation
// 分成独立生命周期，供 Mock 验收使用；不拥有设备、线程、文件或网络资源。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../domain/audio_frame.hpp"
#include "../domain/error.hpp"

namespace nexweave::runtime {

// v1 输入事件类型。事件序号属于 stream_id，generation 只属于回答输出代际。
enum class InputEventKind : std::uint8_t {
  kStart,
  kFrame,
  kEnd,
  kCancel
};

// 固定帧输入的版本化事件。kFrame 必须携带 320 个样本；kEnd 可携带 0 个样本，
// 或携带一帧补零后的短尾及 1..319 的 valid_samples。接收方拥有 optional 帧副本。
struct InputStreamEvent {
  std::uint32_t version = 1;
  std::string stream_id;
  std::uint64_t generation = 0;
  std::uint64_t sequence = 0;
  InputEventKind kind = InputEventKind::kStart;
  std::optional<domain::AudioFrame> frame;
  std::size_t valid_samples = 0;
};

// 检查版本、stream/sequence、事件字段、固定帧和尾部约束；未知 kind、缺帧、短帧或
// valid_samples 越界返回 kInvalidInput。纯值校验不记录历史，不阻塞，也不修改输入。
domain::OperationResult validate_input_event(const InputStreamEvent& event);

// 生产者把任意长度样本切成 320 样本帧；最后不足一帧时补零并返回有效样本数。
// 输入 0 个样本返回空列表；不会静默截断，输出帧拥有独立数据副本。
struct PaddedAudioFrame {
  domain::AudioFrame frame;
  std::size_t valid_samples = 0;
};
domain::Result<std::vector<PaddedAudioFrame>> pad_audio_samples(
    const std::vector<std::int16_t>& samples);

// 粗粒度活动的可审计标记。播放可以在生成结束前开始，三个 done 不互相替代。
enum class ActivityMarker : std::uint8_t {
  kGenerationStarted,
  kPlaybackStarted,
  kGenerationDone,
  kSynthesisDone,
  kPlaybackDone,
  kCancelAccepted,
  kOldOutputBlocked,
  kExecutionExited,
  kPlaybackCleared,
  kTerminalSucceeded,
  kTerminalCancelled
};

// 确定性 fixture 的状态。所有调用串行；每次成功操作追加一个标记，取消后旧 generation
// 的完成标记被拒绝。terminal 只出现一次，且成功必须等待 generation/synthesis/playback done。
class InteractionContractFixture final {
 public:
  // 初始状态无 stream、generation 和终态；对象不创建外部资源。
  InteractionContractFixture() = default;

  // 常驻采集只建立一次 stream；stream_id 非空且不能重复，成功后 sequence 从 0 开始。
  domain::OperationResult start_stream(const std::string& stream_id);
  // 逐帧输入必须属于当前 stream、generation 且 sequence 恰为下一值；固定帧不可短。
  domain::OperationResult push_frame(const InputStreamEvent& event);
  // 结束允许零帧或补零尾帧；结束后不能继续写入该 stream。
  domain::OperationResult end_stream(const InputStreamEvent& event);

  // 新回答推进 generation 并封锁旧输出；输入 stream 生命周期不受影响。
  domain::Result<std::uint64_t> begin_generation();
  // 只允许当前 generation；播放可先于 generation_done 开始，证明流式重叠。
  domain::OperationResult start_playback(std::uint64_t generation);
  domain::OperationResult mark_generation_done(std::uint64_t generation);
  domain::OperationResult mark_synthesis_done(std::uint64_t generation);
  domain::OperationResult mark_playback_done(std::uint64_t generation);
  // 取消先记录受理、封锁旧输出，再记录执行退出和播放清理；不可撤回已播放声音。
  domain::OperationResult cancel(std::uint64_t generation);

  ActivityMarker last_marker() const noexcept;
  std::vector<ActivityMarker> trace() const;
  bool terminal() const noexcept;
  // 当前回答代际水位。调用方（例如 Session 编排）在提交完成标记或取消时必须使用
  // 这个值，而不是自己维护的副本：夹具在 begin_generation() 里推进水位，本地副本
  // 一旦脱节，标记就会被当成旧代际而拒绝。未开启任何代际时返回 0。
  std::uint64_t generation() const noexcept;

 private:
  std::string stream_id_;
  std::uint64_t next_sequence_ = 0;
  bool stream_started_ = false;
  bool stream_ended_ = false;
  std::uint64_t generation_ = 0;
  bool generation_started_ = false;
  bool generation_done_ = false;
  bool synthesis_done_ = false;
  bool playback_started_ = false;
  bool playback_done_ = false;
  bool cancelled_ = false;
  bool terminal_ = false;
  ActivityMarker last_marker_ = ActivityMarker::kGenerationStarted;
  std::vector<ActivityMarker> trace_;
};

}  // namespace nexweave::runtime

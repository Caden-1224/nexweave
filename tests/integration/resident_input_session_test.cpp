#include "../test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fake_asr.hpp"
#include "fake_audio.hpp"
#include "fake_rag.hpp"
#include "fake_tts.hpp"
#include "resident_audio_input.hpp"
#include "session_runtime.hpp"

using namespace nexweave;
using nexweave::runtime::ResidentAudioInput;
using nexweave::runtime::SegmentAvailability;
using nexweave::runtime::SpeechActivity;

namespace {

// 统一音频契约的固定帧长：1 帧 = 320 样本 = 20 ms。本文件所有容量与序号都以帧为单位。
constexpr std::size_t kFrameSamples = domain::kAudioFrameSamples;

// 分段策略。取值刻意取小，使每条用例只需几十帧就能走完起音、静音超时与最长语音，
// 因此不需要真实时间也不需要睡眠。前置缓冲 3 帧 = 60 ms。
runtime::SpeechSegmentationConfig SegmentationConfig() {
  runtime::SpeechSegmentationConfig config;
  config.preroll_frames = 3;
  config.min_silence_frames = 6;
  config.min_speech_frames = 4;
  config.max_speech_frames = 50;
  return config;
}

// 最长语音场景的分段策略：最长语音压到 6 帧，使十几帧的持续人声就会被切成多块，
// 从而让“最长语音”也走完整的常驻输入链路，而不只停留在分段器单元测试里。
runtime::SpeechSegmentationConfig LongSpeechConfig() {
  runtime::SpeechSegmentationConfig config;
  config.preroll_frames = 2;
  config.min_silence_frames = 4;
  config.min_speech_frames = 3;
  config.max_speech_frames = 6;
  return config;
}

// 场景构建器：把“第 n 段有多少帧、是人声还是静音、样本取什么值”写成显式脚本，
// 同时生成活动脚本与 PCM 内容。静音与人声取不同的样本值，从而可以在“新段开头被保留”
// 的断言里核对真实帧内容，而不是只数帧数。
struct Region {
  std::size_t frames;
  SpeechActivity activity;
  std::int16_t value;
};

struct Scenario {
  std::vector<std::int16_t> samples;
  std::vector<runtime::ActivityRun> runs;
  std::size_t frame_count = 0;
};

Scenario BuildScenario(const std::vector<Region>& regions) {
  Scenario scenario;
  for (const auto& region : regions) {
    scenario.runs.push_back(runtime::ActivityRun{region.frames, region.activity});
    scenario.frame_count += region.frames;
    scenario.samples.insert(scenario.samples.end(), region.frames * kFrameSamples, region.value);
  }
  return scenario;
}

// 一段完整说话：起音前静音 4 帧、人声 6 帧、结束后静音 6 帧，共 16 帧。
// 静音 6 帧正好达到静音超时，因此第 16 帧交付一个段。
void AppendUtterance(std::vector<Region>& regions, std::int16_t silence, std::int16_t speech) {
  regions.push_back(Region{4, SpeechActivity::kSilence, silence});
  regions.push_back(Region{6, SpeechActivity::kSpeech, speech});
  regions.push_back(Region{6, SpeechActivity::kSilence, silence});
}

constexpr std::int16_t kSilenceSampleA = 111;
constexpr std::int16_t kSilenceSampleB = 222;
constexpr std::int16_t kSpeechSampleA = 3000;
constexpr std::int16_t kSpeechSampleB = 6000;

// L1 直答文本固定 30 字节 = 2 帧（⌈30/16⌉），因此一轮回答的播放边界恰好两次。
constexpr char kL1Answer[] = "the capital of france is paris";
constexpr std::size_t kAnswerFrames = 2;

std::string QuestionText() {
  return std::string("capital of france");
}

// L1 路由命中夹具：片段文本包含识别文本，符合确定性检索器的匹配方向。
backend::FakeRag MakeIndex() {
  std::vector<capability::RetrievedChunk> chunks;
  chunks.push_back(capability::RetrievedChunk{"geo-capital-fr", kL1Answer, 0.97});
  chunks.push_back(capability::RetrievedChunk{"weather", "sunny fixture weather", 0.20});
  return backend::FakeRag(std::move(chunks));
}

// 记录送帧内容并输出固定识别文本的 ASR 替身。它证明“被保留的新语音确实作为下一轮的
// 输入进入识别”，而不是只检查段对象还在队列里；set_callback 在每轮开始时清空记录，
// 因此 frames() 永远只描述最近一轮。
class RecordingAsr final : public capability::IAsr {
 public:
  domain::OperationResult set_callback(capability::TextEventCallback callback) override {
    callback_ = std::move(callback);
    frames_.clear();
    last_count_ = 0;
    return domain::OperationResult::success();
  }

  domain::OperationResult feed(const domain::AudioFrame& frame, bool is_last) override {
    if (!callback_) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput, "未注册回调");
    }
    frames_.push_back(frame);
    if (is_last) {
      ++last_count_;
      capability::TextEvent event;
      event.kind = capability::TextEventKind::kFinal;
      event.text = QuestionText();
      callback_(event);
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    return domain::OperationResult::success();
  }

  const std::vector<domain::AudioFrame>& frames() const {
    return frames_;
  }

  std::size_t last_count() const {
    return last_count_;
  }

 private:
  capability::TextEventCallback callback_;
  std::vector<domain::AudioFrame> frames_;
  std::size_t last_count_ = 0;
};

// 会在读满 good_frames 帧之后报设备错误的音频源：用于验证“源失败时未完成的说话被
// 放弃，但此前已经完整交付的段仍然有效”，即失败不会连累已经确认的音频。
class FailingSource final : public capability::IAudioSource {
 public:
  FailingSource(std::vector<std::int16_t> samples, std::size_t good_frames)
      : samples_(std::move(samples)), good_frames_(good_frames) {}

  domain::OperationResult open() override {
    cursor_ = 0;
    read_frames_ = 0;
    opened_ = true;
    return domain::OperationResult::success();
  }

  domain::Result<domain::AudioFrame> read() override {
    if (!opened_) {
      return domain::Result<domain::AudioFrame>::failure(domain::ErrorCode::kDeviceFailure,
                                                         "音频源未打开");
    }
    if (read_frames_ >= good_frames_) {
      return domain::Result<domain::AudioFrame>::failure(domain::ErrorCode::kDeviceFailure,
                                                         "采集设备读取失败");
    }
    std::vector<std::int16_t> block(samples_.begin() + static_cast<std::ptrdiff_t>(cursor_),
                                    samples_.begin() +
                                        static_cast<std::ptrdiff_t>(cursor_ + kFrameSamples));
    cursor_ += kFrameSamples;
    ++read_frames_;
    const auto frame = domain::AudioFrame::from_samples(block);
    if (!frame.ok()) {
      return domain::Result<domain::AudioFrame>::failure(domain::ErrorCode::kDeviceFailure,
                                                         "夹具帧非法");
    }
    return domain::Result<domain::AudioFrame>::success(frame.value);
  }

  domain::OperationResult cancel() noexcept override {
    return domain::OperationResult::success();
  }

  domain::OperationResult close() noexcept override {
    opened_ = false;
    return domain::OperationResult::success();
  }

 private:
  std::vector<std::int16_t> samples_;
  std::size_t good_frames_ = 0;
  std::size_t cursor_ = 0;
  std::size_t read_frames_ = 0;
  bool opened_ = false;
};

// 会话指针的延迟绑定：播放设备必须在会话构造之前就存在（会话借用设备），因此先构造设备，
// 会话构造完成后再回填指针。这里用它读取“泵帧时会话处于什么阶段”，把“采集在播报期间继续
// 生产”从一句推理变成一条可断言的证据。
struct SessionSlot {
  runtime::SessionRuntime* session = nullptr;
};

// 会话播放设备替身：按实时速率推进逻辑时钟，并可在指定的写出序号上泵入常驻输入。
// 这样“设备正在播报时用户又开始说话”就发生在确定的帧边界上，不需要线程与睡眠。
class PumpingSink final : public capability::IAudioSink {
 public:
  PumpingSink(runtime::IPlaybackClock& clock, ResidentAudioInput& input, SessionSlot& slot)
      : clock_(clock), input_(input), slot_(slot) {}

  // 在第 trigger_write 次写出之后泵入 pump_frames 帧；trigger_write 为 0 表示不泵帧。
  void ArmAt(std::size_t trigger_write, std::size_t pump_frames) {
    trigger_write_ = trigger_write;
    pump_frames_ = pump_frames;
  }

  domain::OperationResult open() override {
    return domain::OperationResult::success();
  }

  domain::OperationResult write(const domain::AudioFrame& frame) override {
    samples_.insert(samples_.end(), frame.samples.begin(), frame.samples.end());
    ++writes_;
    // 真实声卡按实时速率消耗音频：每写出一帧（20 ms）就有一帧时长的音频播完，
    // 因此一轮回答可以在同一次 run() 里播完，与设备时间冻结的场景形成对照。
    clock_.advance(domain::kAudioFrameDurationMs);
    if (trigger_write_ != 0 && writes_ == trigger_write_ && pump_frames_ > 0) {
      // 泵帧只进入常驻输入，不回调会话：会话在自己的播放交付边界上消费起音通知，
      // 打断因此有唯一的线性化点，不存在从设备内部重入会话的隐藏路径。
      const auto pumped = input_.pump(pump_frames_);
      CHECK(pumped.ok());
      pumped_frames_ += *pumped.value;
      if (slot_.session != nullptr) {
        // 记录泵帧那一刻的会话阶段：用例据此证明采集确实在播报期间继续生产。
        state_at_pump_ = slot_.session->state_machine().state();
      }
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    return domain::OperationResult::success();
  }

  domain::OperationResult close() noexcept override {
    return domain::OperationResult::success();
  }

  std::size_t write_count() const noexcept {
    return writes_;
  }

  std::size_t sample_count() const noexcept {
    return samples_.size();
  }

  std::size_t pumped_frames() const noexcept {
    return pumped_frames_;
  }

  // 最近一次泵帧时的会话阶段；未泵过帧时保持默认值。
  runtime::SessionStateMachine::State state_at_pump() const noexcept {
    return state_at_pump_;
  }

 private:
  runtime::IPlaybackClock& clock_;
  ResidentAudioInput& input_;
  SessionSlot& slot_;
  std::vector<std::int16_t> samples_;
  std::size_t writes_ = 0;
  std::size_t pumped_frames_ = 0;
  std::size_t trigger_write_ = 0;
  std::size_t pump_frames_ = 0;
  runtime::SessionStateMachine::State state_at_pump_ = runtime::SessionStateMachine::State::kIdle;
};

// 会话 + 播放 + 常驻输入的组合夹具。成员声明顺序即构造顺序：路由依赖索引、播放依赖
// 设备与时钟、会话依赖其余全部，因此这里不能用更晚构造的成员。
struct SessionStack {
  SessionStack(capability::IAsr& asr, ResidentAudioInput& input)
      : router(rag, backend::FakeRagRouter::Config{0.90, 0.50, 3}),
        sink(clock, input, slot),
        playback(sink, clock),
        session(asr, rag, router, tts, playback) {}

  // 会话构造完成后调用一次，把会话指针回填给设备；不调用时设备只泵帧、不读会话阶段。
  void bind() {
    slot.session = &session;
  }

  backend::FakeRag rag = MakeIndex();
  backend::FakeRagRouter router;
  backend::FakeTts tts;
  runtime::ManualPlaybackClock clock;
  SessionSlot slot;
  PumpingSink sink;
  runtime::LogicalClockPlayback playback;
  runtime::SessionRuntime session;
};

// 把常驻输入里的一段语音变成一轮会话输入。流标识取自段本身，保证“新段开头有归属”
// 在会话侧也成立，而不是由调用方另编一个名字。
runtime::SessionTurnInput TurnFrom(const runtime::SpeechSegment& segment,
                                   const std::string& request_id) {
  runtime::SessionTurnInput input;
  input.stream_id = segment.stream_id;
  input.request_id = request_id;
  input.generation = 1;
  input.pcm_samples = runtime::segment_samples(segment);
  return input;
}

// 由脚本、源、判定器与常驻输入组成的输入侧夹具。
struct InputStack {
  explicit InputStack(Scenario scenario, std::size_t pending_capacity = 4)
      : InputStack(std::move(scenario), SegmentationConfig(), pending_capacity) {}

  InputStack(Scenario scenario, runtime::SpeechSegmentationConfig segmentation,
             std::size_t pending_capacity)
      : script(std::move(scenario.runs)),
        source(std::move(scenario.samples)),
        input(source, script, kStreamId, MakeConfig(segmentation, pending_capacity)) {}

  static runtime::ResidentAudioInputConfig MakeConfig(
      const runtime::SpeechSegmentationConfig& segmentation, std::size_t pending_capacity) {
    runtime::ResidentAudioInputConfig config;
    config.segmentation = segmentation;
    config.pending_segment_capacity = pending_capacity;
    return config;
  }

  static constexpr const char* kStreamId = "mic-resident";

  runtime::SpeechActivityScript script;
  backend::FakeAudioSource source;
  ResidentAudioInput input;
};

}  // namespace

// 采集生命周期不变量：一条常驻输入跨越多轮回答持续生产；回答正常收尾不关闭采集，
// 只有源读空（输入自然结束）或显式停止才结束。结束之后再取段得到明确的 kEnded，
// 重复结束返回 kAlreadyCompleted 且不重复交付。
void TestCaptureSurvivesTurnsUntilExplicitEnd() {
  std::vector<Region> regions;
  AppendUtterance(regions, kSilenceSampleA, kSpeechSampleA);
  AppendUtterance(regions, kSilenceSampleB, kSpeechSampleB);
  InputStack stack(BuildScenario(regions));
  CHECK(stack.input.start().ok());
  CHECK(stack.input.capturing());
  // 开始之后不允许再次开始：一条输入流只有一个拥有者，重复开始会让序号来源分裂。
  CHECK(!stack.input.start().ok());
  CHECK(stack.input.start().error.code == domain::ErrorCode::kAlreadyCompleted);

  // 分两次泵完全部 32 帧：第一次覆盖第一段（第 16 帧交付），第二次覆盖第二段。
  // 预算用尽不等于输入结束——源还没被读空，采集必须继续存活。
  const auto first_pump = stack.input.pump(16);
  CHECK(first_pump.ok());
  CHECK(*first_pump.value == 16);
  CHECK(stack.source.frame_count() == 32);
  CHECK(stack.input.capturing());
  CHECK(stack.input.queued_segments() == 1);
  const auto second_pump = stack.input.pump(16);
  CHECK(second_pump.ok());
  CHECK(*second_pump.value == 16);
  CHECK(stack.input.capturing());
  CHECK(stack.input.queued_segments() == 2);
  CHECK(stack.input.pending_segments() == 2);
  CHECK(stack.input.dropped_segments() == 0);

  backend::FakeAsr asr({QuestionText()});
  SessionStack session(asr, stack.input);
  CHECK(session.sink.open().ok());

  const auto first = stack.input.take_segment();
  CHECK(first.availability == SegmentAvailability::kAvailable);
  CHECK(first.segment.segment_id == 1);
  const auto first_turn = session.session.run(TurnFrom(first.segment, "req-1"));
  CHECK(first_turn.error.ok());
  CHECK(first_turn.completed);
  // 一轮回答结束不关闭采集：常驻输入还能继续供应下一句话。
  CHECK(stack.input.capturing());

  const auto second = stack.input.take_segment();
  CHECK(second.availability == SegmentAvailability::kAvailable);
  CHECK(second.segment.segment_id == 2);
  const auto second_turn = session.session.run(TurnFrom(second.segment, "req-2"));
  CHECK(second_turn.error.ok());
  CHECK(second_turn.completed);
  CHECK(stack.input.capturing());
  CHECK(stack.input.pending_segments() == 0);

  // 源已经读空：下一次泵帧读到 EOF，输入自然结束并唤醒等待者。
  const auto exhausted = stack.input.pump(8);
  CHECK(exhausted.ok());
  CHECK(*exhausted.value == 0);
  CHECK(!stack.input.capturing());
  CHECK(stack.input.last_error().ok());
  const auto ended = stack.input.take_segment();
  CHECK(ended.availability == SegmentAvailability::kEnded);
  // 重复结束被拒绝，段计数不变，证明结束路径不会重复冲刷。
  CHECK(!stack.input.end_input().ok());
  CHECK(stack.input.end_input().error.code == domain::ErrorCode::kAlreadyCompleted);
  CHECK(stack.input.queued_segments() == 2);
  // 会话侧的正常收尾：关闭常驻输入流是一次性动作，重复调用失败但不破坏已有证据。
  CHECK(session.session.finish_stream().ok());
  CHECK(!session.session.finish_stream().ok());
}

// 打断不变量（本变更的核心验收）：播报期间出现新语音时，会话在同一帧的播放边界上
// 触发旧回答取消；新语音的开头（含段头前置缓冲）完整保留在常驻输入里，并作为下一轮
// 的真实输入进入识别。它同时证明“打断不停采集、不删新语音”。
void TestSpeechDuringPlaybackInterruptsAndPreservesNewSegment() {
  std::vector<Region> regions;
  AppendUtterance(regions, kSilenceSampleA, kSpeechSampleA);
  AppendUtterance(regions, kSilenceSampleB, kSpeechSampleB);
  InputStack stack(BuildScenario(regions));
  CHECK(stack.input.start().ok());

  // 先只泵第一段说话（16 帧），把它作为第一轮的输入。
  const auto pumped = stack.input.pump(16);
  CHECK(pumped.ok());
  CHECK(*pumped.value == 16);
  const auto first = stack.input.take_segment();
  CHECK(first.availability == SegmentAvailability::kAvailable);
  CHECK(first.segment.segment_id == 1);

  RecordingAsr asr;
  SessionStack session(asr, stack.input);
  CHECK(session.sink.open().ok());
  session.session.set_barge_in_monitor(&stack.input);
  session.bind();

  // 第 1 帧播出之后泵入第 17～21 帧：新说话起音前静音 4 帧 + 起音帧本身。
  // 这一帧人声就是新说话起音，属于第 2 段，段起点被前置缓冲回推到第 17 帧。
  session.sink.ArmAt(1, 5);
  const auto interrupted = session.session.run(TurnFrom(first.segment, "req-barge-in"));

  CHECK(interrupted.cancelled);
  CHECK(!interrupted.completed);
  CHECK(interrupted.interrupted_by_speech);
  CHECK(!interrupted.playback_done);
  CHECK(interrupted.terminal_marker == runtime::ActivityMarker::kTerminalCancelled);
  CHECK(interrupted.interrupt_notice.stream_id == InputStack::kStreamId);
  CHECK(interrupted.interrupt_notice.segment_id == 2);
  CHECK(interrupted.interrupt_notice.start_sequence == 17);
  CHECK(interrupted.interrupt_notice.speech_sequence == 20);
  // 打断发生在第 1 帧之后：第 1 帧已经播出并保留，第 2 帧从未交付给设备。
  CHECK(session.sink.write_count() == 1);
  CHECK(session.playback.played_count() == 1);
  CHECK(session.sink.pumped_frames() == 5);
  // 泵帧发生在会话处于 Speaking 的时刻：这正是“回答还没播完，采集仍在继续生产”的
  // 直接证据，而不是靠“常驻输入与会话没有耦合”这句话去推断。
  CHECK(session.sink.state_at_pump() == runtime::SessionStateMachine::State::kSpeaking);
  // 打断只作废旧回答的输出，不关闭采集。
  CHECK(stack.input.capturing());

  // 继续泵完剩余帧：新说话在静音超时处交付，源随后读空并自然结束输入。
  const auto rest = stack.input.pump(64);
  CHECK(rest.ok());
  CHECK(*rest.value == 11);
  CHECK(!stack.input.capturing());

  const auto next = stack.input.take_segment();
  CHECK(next.availability == SegmentAvailability::kAvailable);
  CHECK(next.segment.segment_id == 2);
  CHECK(next.segment.chunk_index == 0);
  CHECK(next.segment.start_sequence == 17);
  CHECK(next.segment.speech_frames == 6);
  CHECK(next.segment.frames.size() == 9);  // 3 帧段头前置缓冲 + 6 帧人声
  CHECK(next.segment.frames.front().samples.front() == kSilenceSampleB);
  CHECK(next.segment.frames.at(2).samples.front() == kSilenceSampleB);
  CHECK(next.segment.frames.at(3).samples.front() == kSpeechSampleB);
  CHECK(next.segment.frames.back().samples.front() == kSpeechSampleB);

  // 新语音没有被任何一轮清理吃掉：它作为下一轮输入逐帧进入识别，内容与段完全一致。
  const auto resumed = session.session.run(TurnFrom(next.segment, "req-resumed"));
  CHECK(resumed.error.ok());
  CHECK(resumed.completed);
  CHECK(!resumed.interrupted_by_speech);
  CHECK(asr.last_count() == 1);
  CHECK(asr.frames().size() == next.segment.frames.size());
  for (std::size_t index = 0; index < asr.frames().size(); ++index) {
    CHECK(asr.frames().at(index).samples == next.segment.frames.at(index).samples);
  }
  CHECK(session.playback.played_count() == kAnswerFrames);
}

// 短脉冲不变量：起音通知在起音帧立即发出（打断必须快，不等确认），但整段人声不足
// 下限的说话不交付成一轮回答。丢弃次数可观测，因此这不是静默丢失。
void TestShortPulseInterruptsWithoutProducingATurn() {
  std::vector<Region> regions;
  AppendUtterance(regions, kSilenceSampleA, kSpeechSampleA);
  // 短脉冲：人声只有 2 帧，低于 4 帧下限。
  regions.push_back(Region{3, SpeechActivity::kSilence, kSilenceSampleB});
  regions.push_back(Region{2, SpeechActivity::kSpeech, kSpeechSampleB});
  regions.push_back(Region{6, SpeechActivity::kSilence, kSilenceSampleB});
  // 脉冲之后用户重新完整说一句。
  AppendUtterance(regions, kSilenceSampleB, kSpeechSampleB);
  InputStack stack(BuildScenario(regions));
  CHECK(stack.input.start().ok());

  const auto pumped = stack.input.pump(16);
  CHECK(pumped.ok());
  const auto first = stack.input.take_segment();
  CHECK(first.segment.segment_id == 1);

  backend::FakeAsr asr({QuestionText()});
  SessionStack session(asr, stack.input);
  CHECK(session.sink.open().ok());
  session.session.set_barge_in_monitor(&stack.input);
  session.sink.ArmAt(1, 4);  // 静音 3 帧 + 脉冲第 1 帧人声
  const auto interrupted = session.session.run(TurnFrom(first.segment, "req-pulse"));
  CHECK(interrupted.cancelled);
  CHECK(interrupted.interrupted_by_speech);
  CHECK(interrupted.interrupt_notice.segment_id == 2);
  CHECK(interrupted.interrupt_notice.start_sequence == 16);

  const auto rest = stack.input.pump(64);
  CHECK(rest.ok());
  CHECK(!stack.input.capturing());
  CHECK(stack.input.dropped_short_segments() == 1);
  CHECK(stack.input.queued_segments() == 2);  // 只交付第 1 段与脉冲后的那一句

  // 脉冲占用了段编号 2 但没有交付，因此下一段拿到编号 3：编号单调说明丢弃是显式事件，
  // 而不是让两次不同的说话共用一个身份。
  const auto next = stack.input.take_segment();
  CHECK(next.availability == SegmentAvailability::kAvailable);
  CHECK(next.segment.segment_id == 3);
  // 完整一句从第 28 帧开始：起音在第 31 帧，段头回推 3 帧前置缓冲。
  CHECK(next.segment.start_sequence == 28);
  CHECK(next.segment.frames.size() == 9);

  const auto resumed = session.session.run(TurnFrom(next.segment, "req-pulse-resumed"));
  CHECK(resumed.error.ok());
  CHECK(resumed.completed);
}

// 取消与新段开始的竞态：起音恰好发生在最后一帧的播放边界上。答案的音频已经全部播完，
// 但打断是主动作废而不是播放失败，因此本轮仍以取消收尾，且不得报告成功终态。
void TestSpeechStartOnLastPlaybackBoundaryStillCancels() {
  std::vector<Region> regions;
  AppendUtterance(regions, kSilenceSampleA, kSpeechSampleA);
  AppendUtterance(regions, kSilenceSampleB, kSpeechSampleB);
  InputStack stack(BuildScenario(regions));
  CHECK(stack.input.start().ok());
  const auto pumped = stack.input.pump(16);
  CHECK(pumped.ok());
  const auto first = stack.input.take_segment();

  backend::FakeAsr asr({QuestionText()});
  SessionStack session(asr, stack.input);
  CHECK(session.sink.open().ok());
  session.session.set_barge_in_monitor(&stack.input);
  // 直答共 2 帧，因此第 2 次写出就是最后一帧的播放边界；此时泵入 5 帧即包含新起音。
  session.sink.ArmAt(kAnswerFrames, 5);

  const auto result = session.session.run(TurnFrom(first.segment, "req-last-frame"));
  CHECK(result.cancelled);
  CHECK(!result.completed);
  CHECK(result.interrupted_by_speech);
  // 两帧都交付并播完，但本轮不因此变成成功：打断作废了这次回答的收尾。
  CHECK(result.pcm_frames.size() == kAnswerFrames);
  CHECK(session.playback.played_count() == kAnswerFrames);
  CHECK(!result.playback_done);
  CHECK(result.terminal_marker == runtime::ActivityMarker::kTerminalCancelled);
  const auto markers = session.session.trace();
  CHECK(std::count(markers.begin(), markers.end(),
                   runtime::ActivityMarker::kTerminalSucceeded) == 0);
}

// 等待队列溢出策略：上限满时丢弃最旧的等待段并计数。最旧的一段是已经过期的用户意图，
// 保留最新一段才能回答用户刚说的话；丢弃可观测，因此不是静默丢帧。
void TestPendingQueueOverflowDropsOldestAndCounts() {
  std::vector<Region> regions;
  AppendUtterance(regions, 10, 1000);
  AppendUtterance(regions, 20, 2000);
  AppendUtterance(regions, 30, 3000);
  InputStack stack(BuildScenario(regions), 1);
  CHECK(stack.input.start().ok());

  // 恰好读完 48 帧，源还没有被读空：输入仍在采集，队列只是满而暂时没有新段。
  const auto pumped = stack.input.pump(48);
  CHECK(pumped.ok());
  CHECK(*pumped.value == 48);
  CHECK(stack.input.queued_segments() == 3);
  CHECK(stack.input.dropped_segments() == 2);
  CHECK(stack.input.pending_segments() == 1);
  CHECK(stack.input.capturing());

  const auto kept = stack.input.take_segment();
  CHECK(kept.availability == SegmentAvailability::kAvailable);
  CHECK(kept.segment.segment_id == 3);
  CHECK(kept.segment.frames.front().samples.front() == 30);
  // 队列取空但输入仍在采集：这是 kPending（继续泵帧），不是 kEnded（结束循环）。
  const auto empty = stack.input.take_segment();
  CHECK(empty.availability == SegmentAvailability::kPending);
}

// 通知合并不变量：会话来不及消费时，多条未消费的起音通知合并成最早的一条并计数。
// 合并保留的是“最早那次说话”的归属——它是本次打断的直接原因；计数则让“发生过不止
// 一次起音”这一事实仍然可观测。合并只影响归属，不影响音频：两段都还在队列里。
void TestUnconsumedSpeechStartsMergeIntoEarliest() {
  std::vector<Region> regions;
  AppendUtterance(regions, kSilenceSampleA, kSpeechSampleA);
  AppendUtterance(regions, kSilenceSampleB, kSpeechSampleB);
  InputStack stack(BuildScenario(regions));
  CHECK(stack.input.start().ok());

  const auto pumped = stack.input.pump(32);
  CHECK(pumped.ok());
  CHECK(*pumped.value == 32);
  CHECK(stack.input.queued_segments() == 2);
  CHECK(stack.input.merged_speech_starts() == 1);

  const auto notice = stack.input.take_speech_started();
  CHECK(notice.has_value());
  CHECK(notice->segment_id == 1);
  CHECK(notice->start_sequence == 1);
  CHECK(notice->speech_sequence == 4);
  CHECK(!stack.input.take_speech_started().has_value());
  // 两段音频都还在等待被取走：通知合并不会连带丢弃新语音。
  CHECK(stack.input.pending_segments() == 2);
  CHECK(stack.input.dropped_segments() == 0);
}

// 最长语音端到端不变量：一次说得很久的说话会被切成多块交付，各块共享同一段编号、块编号
// 递增、且不产生新的起音通知（否则设备会把同一次说话误判成多次打断）；按块拼接后音频与
// 原始输入一致，并且能作为一轮完整输入进入会话。这条用例让“最长语音”不再只由分段器
// 单元测试覆盖，而是走完整条常驻输入链路。
void TestLongSpeechIsChunkedWithoutRepeatedBargeIn() {
  std::vector<Region> regions;
  regions.push_back(Region{3, SpeechActivity::kSilence, kSilenceSampleA});
  regions.push_back(Region{14, SpeechActivity::kSpeech, kSpeechSampleA});
  regions.push_back(Region{4, SpeechActivity::kSilence, kSilenceSampleA});
  InputStack stack(BuildScenario(regions), LongSpeechConfig(), 4);
  CHECK(stack.input.start().ok());

  const auto pumped = stack.input.pump(21);
  CHECK(pumped.ok());
  CHECK(*pumped.value == 21);
  CHECK(stack.input.queued_segments() == 3);

  const auto first = stack.input.take_segment();
  CHECK(first.availability == SegmentAvailability::kAvailable);
  CHECK(first.segment.segment_id == 1);
  CHECK(first.segment.chunk_index == 0);
  CHECK(first.segment.cut_reason == runtime::SegmentCutReason::kMaxSpeech);
  CHECK(first.segment.frames.size() == 8);  // 2 帧段头前置缓冲 + 6 帧人声
  CHECK(first.segment.speech_frames == 6);
  CHECK(first.segment.start_sequence == 1);

  const auto second = stack.input.take_segment();
  CHECK(second.segment.segment_id == 1);
  CHECK(second.segment.chunk_index == 1);
  CHECK(second.segment.cut_reason == runtime::SegmentCutReason::kMaxSpeech);
  CHECK(second.segment.frames.size() == 6);
  CHECK(second.segment.speech_frames == 6);
  // 续块沿用段起点：start_sequence 标识“这次说话从哪里开始”，不是本块第一帧的序号。
  CHECK(second.segment.start_sequence == 1);

  const auto third = stack.input.take_segment();
  CHECK(third.segment.segment_id == 1);
  CHECK(third.segment.chunk_index == 2);
  CHECK(third.segment.cut_reason == runtime::SegmentCutReason::kSilenceTimeout);
  CHECK(third.segment.frames.size() == 2);
  CHECK(third.segment.speech_frames == 2);

  // 一次说话只产生一条起音通知：切块不是新的说话开始，因此不会造成重复打断。
  const auto notice = stack.input.take_speech_started();
  CHECK(notice.has_value());
  CHECK(notice->segment_id == 1);
  CHECK(notice->start_sequence == 1);
  CHECK(notice->speech_sequence == 3);
  CHECK(!stack.input.take_speech_started().has_value());
  CHECK(stack.input.merged_speech_starts() == 0);

  // 按块拼接还原这段说话：14 帧人声 + 2 帧段头前置缓冲 = 16 帧，且内容逐帧对应原脚本。
  runtime::SpeechSegment joined = first.segment;
  joined.frames.insert(joined.frames.end(), second.segment.frames.begin(),
                       second.segment.frames.end());
  joined.frames.insert(joined.frames.end(), third.segment.frames.begin(),
                       third.segment.frames.end());
  CHECK(joined.frames.size() == 16);
  const auto samples = runtime::segment_samples(joined);
  CHECK(samples.size() == 16 * kFrameSamples);
  CHECK(samples.front() == kSilenceSampleA);
  CHECK(samples.at(2 * kFrameSamples) == kSpeechSampleA);
  CHECK(samples.back() == kSpeechSampleA);

  // 拼接结果作为一轮完整输入进入会话：切块没有让音频走进死路。
  backend::FakeAsr asr({QuestionText()});
  SessionStack session(asr, stack.input);
  CHECK(session.sink.open().ok());
  const auto turn = session.session.run(TurnFrom(joined, "req-long-speech"));
  CHECK(turn.error.ok());
  CHECK(turn.completed);
}

// 唤醒不变量：消费线程阻塞等待时，输入结束必须把它唤醒并给出明确结论，而不是让它等到
// 超时。三种线程交错都必须得到同一结论——先拿到段，再拿到“已结束”——因此用例只断言
// 结果，不依赖调度顺序，也不使用 sleep。
void TestWaiterIsWokenByInputEnd() {
  std::vector<Region> regions;
  AppendUtterance(regions, kSilenceSampleA, kSpeechSampleA);
  InputStack stack(BuildScenario(regions));
  CHECK(stack.input.start().ok());

  std::thread producer([&stack] {
    while (true) {
      const auto pumped = stack.input.pump(8);
      if (!pumped.ok() || *pumped.value == 0) {
        return;
      }
    }
  });

  const auto first = stack.input.wait_for_segment();
  CHECK(first.availability == SegmentAvailability::kAvailable);
  CHECK(first.segment.segment_id == 1);
  const auto second = stack.input.wait_for_segment();
  CHECK(second.availability == SegmentAvailability::kEnded);
  producer.join();
  CHECK(!stack.input.capturing());
}

// 超时边界：既没有新段、输入也没有结束时，带超时的等待返回 kTimeout，而不是伪造空段
// 或把“暂时没有”说成“已经结束”；一旦输入结束，同一个入口立即返回 kEnded。
void TestWaitForSegmentTimesOutWhenIdle() {
  std::vector<Region> regions;
  AppendUtterance(regions, kSilenceSampleA, kSpeechSampleA);
  InputStack stack(BuildScenario(regions));
  CHECK(stack.input.start().ok());

  // 一帧都不泵：等待队列为空，输入仍在采集。
  const auto idle = stack.input.wait_for_segment(30);
  CHECK(idle.availability == SegmentAvailability::kTimeout);
  CHECK(idle.segment.frames.empty());
  CHECK(stack.input.capturing());

  // 输入结束后同一个入口立即返回 kEnded，不再消耗完整超时。
  CHECK(stack.input.end_input().ok());
  const auto ended = stack.input.wait_for_segment(5000);
  CHECK(ended.availability == SegmentAvailability::kEnded);
  // 无超时重载对同一终态给出同样结论，两个重载共用同一组状态。
  const auto ended_again = stack.input.wait_for_segment();
  CHECK(ended_again.availability == SegmentAvailability::kEnded);
}

// 活动输入下退出：用户在说话过程中显式停止输入。正在进行、结尾不可知的说话被放弃
// 并计数，等待者被立即唤醒，采集关闭；重复停止幂等且不改计数。
void TestStopUnderActiveInputDiscardsAndWakesWaiter() {
  std::vector<Region> regions;
  // 只有起音与持续人声，没有结束静音：停止时说话仍在进行。
  regions.push_back(Region{3, SpeechActivity::kSilence, kSilenceSampleA});
  regions.push_back(Region{6, SpeechActivity::kSpeech, kSpeechSampleA});
  InputStack stack(BuildScenario(regions));
  CHECK(stack.input.start().ok());
  const auto pumped = stack.input.pump(9);
  CHECK(pumped.ok());
  CHECK(*pumped.value == 9);
  CHECK(stack.input.pending_segments() == 0);

  // 先取得“消费者确实停在了等待上”的可观察证据：此刻既没有已完成段、输入也还没有结束，
  // 因此带超时的探测等待只能以 kTimeout 返回。随后改用无超时等待——它只有“取到段”和
  // “输入结束/停止”两种返回可能，而主线程在看到停车证据之前不会停止输入，所以最终返回
  // kEnded 只能由 stop() 的唤醒造成，不可能来自“终态恰好先于等待置位”这条捷径。
  std::atomic<bool> probe_done{false};
  std::atomic<bool> probe_timed_out{false};
  SegmentAvailability outcome = SegmentAvailability::kAvailable;
  std::thread consumer([&stack, &probe_done, &probe_timed_out, &outcome] {
    const auto probe = stack.input.wait_for_segment(20);
    probe_timed_out.store(probe.availability == SegmentAvailability::kTimeout);
    probe_done.store(true);
    const auto woken = stack.input.wait_for_segment();
    outcome = woken.availability;
  });
  const auto probe_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (!probe_done.load() && std::chrono::steady_clock::now() < probe_deadline) {
    std::this_thread::yield();
  }
  CHECK(probe_done.load());
  CHECK(probe_timed_out.load());
  const auto stopped = stack.input.stop();
  consumer.join();

  CHECK(stopped.ok());
  CHECK(outcome == SegmentAvailability::kEnded);
  CHECK(!stack.input.capturing());
  CHECK(stack.input.abandoned_segments() == 1);
  CHECK(stack.input.abandoned_speech_frames() == 6);
  CHECK(stack.input.pending_segments() == 0);

  // 幂等：重复停止不改变任何计数。
  CHECK(stack.input.stop().ok());
  CHECK(stack.input.abandoned_segments() == 1);
  CHECK(stack.input.abandoned_speech_frames() == 6);
  // 已经结束的输入再泵帧不是错误，只是没有新帧。
  const auto again = stack.input.pump(4);
  CHECK(again.ok());
  CHECK(*again.value == 0);
}

// 源失败收敛：读取设备失败时，未完成的说话被放弃（音频连续性不可知，不能当完整一句
// 交付），但此前完整交付的段仍然有效可取，失败原因原样保留。
void TestSourceFailureAbandonsInFlightSpeechAndKeepsCompletedSegments() {
  std::vector<Region> regions;
  AppendUtterance(regions, kSilenceSampleA, kSpeechSampleA);
  regions.push_back(Region{3, SpeechActivity::kSilence, kSilenceSampleB});
  regions.push_back(Region{6, SpeechActivity::kSpeech, kSpeechSampleB});
  const Scenario scenario = BuildScenario(regions);
  runtime::SpeechActivityScript script(scenario.runs);
  FailingSource source(scenario.samples, 16 + 9);
  runtime::ResidentAudioInputConfig config;
  config.segmentation = SegmentationConfig();
  // 显式传入配置：默认分段策略（静音超时 500 ms）下 16 帧不会结束一次说话，
  // 本用例要复现的是“源在说话中途失败”，因此必须用小取值把时序压到几十帧内。
  ResidentAudioInput input(source, script, "mic-failing", config);

  CHECK(input.start().ok());
  const auto first = input.pump(16);
  CHECK(first.ok());
  CHECK(*first.value == 16);
  CHECK(input.pending_segments() == 1);

  const auto failed = input.pump(32);
  CHECK(!failed.ok());
  CHECK(failed.error.code == domain::ErrorCode::kDeviceFailure);
  CHECK(input.last_error().code == domain::ErrorCode::kDeviceFailure);
  CHECK(!input.capturing());
  // 在途说话被放弃：9 帧人声一帧都没有被当成完整一句交付。
  CHECK(input.abandoned_segments() == 1);
  CHECK(input.abandoned_speech_frames() == 6);
  CHECK(input.queued_segments() == 1);

  // 已经完整交付的段不受失败影响：消费方可以把它们取完再看到 kEnded。
  const auto kept = input.take_segment();
  CHECK(kept.availability == SegmentAvailability::kAvailable);
  CHECK(kept.segment.segment_id == 1);
  CHECK(kept.segment.frames.size() == 9);
  const auto drained = input.take_segment();
  CHECK(drained.availability == SegmentAvailability::kEnded);
}

// 预算与序号不变量：单次泵帧只读取预算内的帧，逐帧恰好消费一次；预算为 0 不读设备。
// 它保护“输入拥有者可以控制单次推进的粒度”，这是后续把泵帧接入真实音频回调的前提。
void TestPumpRespectsBudgetAndReadsEachFrameOnce() {
  std::vector<Region> regions;
  AppendUtterance(regions, kSilenceSampleA, kSpeechSampleA);
  InputStack stack(BuildScenario(regions));
  CHECK(stack.input.start().ok());

  const auto zero = stack.input.pump(0);
  CHECK(zero.ok());
  CHECK(*zero.value == 0);
  CHECK(stack.source.frames_read() == 0);

  for (std::size_t index = 0; index < 5; ++index) {
    const auto pumped = stack.input.pump(1);
    CHECK(pumped.ok());
    CHECK(*pumped.value == 1);
  }
  CHECK(stack.source.frames_read() == 5);
  CHECK(stack.input.frames_pumped() == 5);
  CHECK(stack.input.pending_segments() == 0);
  // 第 5 帧（下标 4）就是起音帧：起音通知立刻可见，段本身却要等静音超时才交付。
  const auto notice = stack.input.take_speech_started();
  CHECK(notice.has_value());
  CHECK(notice->speech_sequence == 4);
  CHECK(notice->start_sequence == 1);
  CHECK(notice->segment_id == 1);
  // 同一段说话只产生一条起音通知；取走之后不会重复通知。
  CHECK(!stack.input.take_speech_started().has_value());
  CHECK(stack.input.merged_speech_starts() == 0);
}

int main() {
  TestCaptureSurvivesTurnsUntilExplicitEnd();
  TestSpeechDuringPlaybackInterruptsAndPreservesNewSegment();
  TestShortPulseInterruptsWithoutProducingATurn();
  TestSpeechStartOnLastPlaybackBoundaryStillCancels();
  TestPendingQueueOverflowDropsOldestAndCounts();
  TestUnconsumedSpeechStartsMergeIntoEarliest();
  TestLongSpeechIsChunkedWithoutRepeatedBargeIn();
  TestWaitForSegmentTimesOutWhenIdle();
  TestWaiterIsWokenByInputEnd();
  TestStopUnderActiveInputDiscardsAndWakesWaiter();
  TestSourceFailureAbandonsInFlightSpeechAndKeepsCompletedSegments();
  TestPumpRespectsBudgetAndReadsEachFrameOnce();
  return 0;
}

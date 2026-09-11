#include "../test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "fake_asr.hpp"
#include "fake_audio.hpp"
#include "fake_rag.hpp"
#include "fake_tts.hpp"
#include "session_runtime.hpp"

using namespace nexweave;
using nexweave::runtime::ActivityMarker;
using nexweave::runtime::SessionStateMachine;
using nexweave::runtime::SessionTurnInput;

namespace {

// 统一音频契约的固定帧长；测试用它构造输入并把“帧数”换算成毫秒。
constexpr std::size_t kFrameSamples = domain::kAudioFrameSamples;

// L1 直答文本固定为 30 字节 = 2 帧（⌈30/16⌉ = 2），帧数可以手算，用来验证
// “合成帧数 = ⌈字节数/16⌉”这条公式在编排层没有被改写。
constexpr char kL1Answer[] = "the capital of france is paris";
constexpr std::size_t kExpectedFrames = 2;

std::vector<std::int16_t> Samples(std::size_t count, std::int16_t value) {
  return std::vector<std::int16_t>(count, value);
}

// 识别文本刻意取成命中文本的子串：Fake 检索器的匹配方向是“片段文本包含查询”，
// 因为文档片段通常比一句问话更长。用真实契约能命中的夹具，用例才聚焦编排本身。
std::string QuestionText() {
  return std::string("capital of france");
}

backend::FakeRag MakeIndex() {
  std::vector<capability::RetrievedChunk> chunks;
  chunks.push_back(capability::RetrievedChunk{"geo-capital-fr", kL1Answer, 0.97});
  chunks.push_back(capability::RetrievedChunk{"weather", "sunny fixture weather", 0.20});
  return backend::FakeRag(std::move(chunks));
}

// 独立复算期望 PCM：同一确定性合成实现、同一文本必须得到逐采样一致的帧序列。
// 编排层只负责搬运帧，不允许改写样本、帧数或元数据；这条断言保护“搬运不改写”，
// 而不是重复断言合成算法本身。
std::vector<domain::AudioFrame> ExpectedFrames(const std::string& text) {
  backend::FakeTts reference;
  std::vector<domain::AudioFrame> frames;
  reference.set_callback([&frames](const domain::AudioFrame& frame) { frames.push_back(frame); });
  const auto synthesized = reference.synthesize(text);
  CHECK(synthesized.ok());
  return frames;
}

// 统计合成调用次数的 TTS 替身：记录调用次数与最后一次文本，不产生任何帧。
// 用于证明“控制意图路径不调用合成”，而不必读取会话的私有状态。
class CountingTts final : public capability::ITts {
 public:
  domain::OperationResult set_callback(capability::AudioEventCallback callback) override {
    callback_ = std::move(callback);
    return domain::OperationResult::success();
  }

  domain::OperationResult synthesize(const std::string& text) override {
    ++calls;
    last_text = text;
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    return domain::OperationResult::success();
  }

  std::size_t calls = 0;
  std::string last_text;

 private:
  capability::AudioEventCallback callback_;
};

// 静止的逻辑时钟：时间只在测试显式推进时前进。用它复现“合成已结束但播放尚未
// 结束”，并把“未播完”和“确实播完”分开。
class TestFrozenClock final : public runtime::IPlaybackClock {
 public:
  std::int64_t now_ms() const noexcept override {
    return now_ms_;
  }

  void advance(std::int64_t delta_ms) override {
    if (delta_ms > 0) {
      now_ms_ += delta_ms;
    }
  }

 private:
  std::int64_t now_ms_ = 0;
};

// 会推进逻辑时钟的设备汇：每次成功写出后逻辑时间前进 20 ms，模拟真实播放按真实
// 时间消耗音频。这样一次 run() 内就能走完“合成 → 播放 → 完成”，并让播放开始标记
// 严格早于合成结束标记。
// 注意：测试不能靠替换 TTS 回调来驱动播放节奏——会话在每轮开始时都会重新注册自己
// 的回调，外部注册的回调会被覆盖，因此节奏必须由注入的设备或时钟提供。
class AdvancingSink : public capability::IAudioSink {
 public:
  explicit AdvancingSink(runtime::IPlaybackClock& clock) : clock_(clock) {}

  domain::OperationResult open() override {
    return domain::OperationResult::success();
  }

  domain::OperationResult write(const domain::AudioFrame& frame) override {
    samples_.insert(samples_.end(), frame.samples.begin(), frame.samples.end());
    ++writes_;
    // 真实声卡按实时速率消耗音频：每写出一帧（20 ms）就有一个帧时长的音频播完。
    // 因此第一帧写出后立即算作播完，第二帧写出后也算作播完，本轮可以在同一次
    // run() 内完成；这与“设备时间不前进”的冻结时钟恰好形成对照。
    clock_.advance(domain::kAudioFrameDurationMs);
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    return domain::OperationResult::success();
  }

  domain::OperationResult close() noexcept override {
    return domain::OperationResult::success();
  }

  std::size_t sample_count() const noexcept {
    return samples_.size();
  }

  std::size_t write_count() const noexcept {
    return writes_;
  }

 protected:
  runtime::IPlaybackClock& clock_;
  std::size_t writes_ = 0;

 private:
  std::vector<std::int16_t> samples_;
};

// 会话指针的延迟绑定：取消用例需要在“设备写出第 1 帧”时请求取消，而设备必须在
// 会话构造完成之前就存在（会话借用设备）。先用占位绑定构造全部对象，再回填会话
// 指针，从而避免循环构造，同时保证运行期指针有效。
struct SessionSlot {
  runtime::SessionRuntime* session = nullptr;
};

// 在第 1 帧播出后请求取消的设备：模拟“用户在新回答刚开始播报时喊停止”。
class CancellingSink : public AdvancingSink {
 public:
  CancellingSink(runtime::IPlaybackClock& clock, SessionSlot& slot) : AdvancingSink(clock), slot_(slot) {}

  domain::OperationResult write(const domain::AudioFrame& frame) override {
    const auto written = AdvancingSink::write(frame);
    if (write_count() == 1 && slot_.session != nullptr) {
      // 第 1 帧已经播出：它属于已播放声音，不可撤回；从下一帧起会话不再交付。
      CHECK(slot_.session->cancel().ok());
    }
    return written;
  }

 private:
  SessionSlot& slot_;
};

}  // namespace

// 成功不变量：一段语音经 ASR 得到文本、经 L1 路由得到有归属的直答，合成 PCM 逐帧
// 交付播放；状态机轨迹固定为 Idle→Listening→Routing→Speaking→Idle（没有 Thinking，
// 因为 L0/L1 不调用 LLM，会话对象根本不持有 LLM 依赖），活动标记顺序固定为
// “文本定稿 → 播放开始 → 合成结束 → 播放结束 → 唯一成功终态”，其中“播放开始早于
// 合成结束”就是流式重叠的可复现证据。设备按实时速率消耗音频，因此一次 run() 内
// 播放可以真正播完；若设备时间不前进，同一路径会依据播放计数报告未完成。
void TestCompletedTurnSequenceAndEvidence() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeAsr asr({QuestionText()});
  backend::FakeTts tts;
  runtime::ManualPlaybackClock clock;
  AdvancingSink sink(clock);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  runtime::SessionRuntime session(asr, rag, router, tts, playback);

  SessionTurnInput input;
  input.stream_id = "mic-l1-ok";
  input.generation = 1;
  input.request_id = "req-l1-ok";
  input.pcm_samples = Samples(kFrameSamples, 7);
  const auto result = session.run(input);

  CHECK(result.error.ok());
  CHECK(result.completed);
  CHECK(!result.cancelled);
  CHECK(result.input_done);
  CHECK(result.playback_started);
  CHECK(result.playback_done);
  // 请求标识原样回显，结果可以归回到具体一次操作。
  CHECK(result.request_id == input.request_id);
  CHECK(result.state == SessionStateMachine::State::kIdle);
  CHECK(result.terminal_marker == ActivityMarker::kTerminalSucceeded);
  CHECK(result.route == backend::RagRouteLevel::kL1);
  CHECK(result.text == kL1Answer);
  CHECK(result.decision.hits.size() == 1);
  CHECK(result.decision.hits.front().id == "geo-capital-fr");

  // 帧数由公式决定：|文本| = 30 字节，⌈30/16⌉ = 2 帧，每帧 320 样本。
  CHECK(std::string(kL1Answer).size() == 30);
  CHECK(result.pcm_frames.size() == kExpectedFrames);
  CHECK(playback.played_count() == kExpectedFrames);
  CHECK(playback.pending_count() == 0);
  for (const auto& frame : result.pcm_frames) {
    CHECK(frame.samples.size() == kFrameSamples);
    CHECK(domain::validate_audio_frame(frame).ok());
  }

  // 独立复算：同一文本的确定性合成结果必须与编排交付的 PCM 逐采样一致。
  const auto reference = ExpectedFrames(kL1Answer);
  CHECK(reference.size() == result.pcm_frames.size());
  for (std::size_t index = 0; index < reference.size(); ++index) {
    CHECK(reference.at(index).samples == result.pcm_frames.at(index).samples);
  }
  CHECK(sink.sample_count() == kExpectedFrames * kFrameSamples);
  CHECK(sink.write_count() == kExpectedFrames);

  const auto transitions = session.state_machine().trace();
  CHECK(transitions.size() == 4);
  CHECK(transitions.at(0) == "idle--audio_start-->listening");
  CHECK(transitions.at(1) == "listening--asr_final-->routing");
  CHECK(transitions.at(2) == "routing--route_l0_l1-->speaking");
  CHECK(transitions.at(3) == "speaking--tts_done-->idle");

  // 六个标记按固定顺序出现：回答文本定稿（generation done）→ 播放开始 → 合成结束
  // → 播放结束 → 唯一成功终态。关键断言是“播放开始”严格早于“合成结束”：这就是
  // 流式重叠的可复现证据，而不是只检查最终同时存在文本和 PCM。
  const auto markers = session.trace();
  CHECK(markers.size() == 6);
  CHECK(markers.at(0) == ActivityMarker::kGenerationStarted);
  CHECK(markers.at(1) == ActivityMarker::kGenerationDone);
  CHECK(markers.at(2) == ActivityMarker::kPlaybackStarted);
  CHECK(markers.at(3) == ActivityMarker::kSynthesisDone);
  CHECK(markers.at(4) == ActivityMarker::kPlaybackDone);
  CHECK(markers.at(5) == ActivityMarker::kTerminalSucceeded);
}

// 合成结束不等于播放结束：帧在合成期间就已经写入设备，但设备时间没有前进，因此
// 一帧都还不算播完。会话必须报告“未完成”并给出明确错误，而不是把合成结束当成
// 播放结束；调用方随后推进设备时间并轮询，同一批音频才算播完。
void TestFrozenClockKeepsPlaybackIncomplete() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeAsr asr({QuestionText()});
  backend::FakeTts tts;
  backend::FakeAudioSink sink;
  CHECK(sink.open().ok());
  TestFrozenClock clock;
  runtime::LogicalClockPlayback playback(sink, clock);
  runtime::SessionRuntime session(asr, rag, router, tts, playback);

  SessionTurnInput input;
  input.stream_id = "mic-frozen";
  input.generation = 1;
  input.request_id = "req-frozen";
  input.pcm_samples = Samples(kFrameSamples, 7);
  const auto result = session.run(input);

  CHECK(!result.error.ok());
  CHECK(result.error.code == domain::ErrorCode::kTimeout);
  CHECK(!result.completed);
  CHECK(result.input_done);
  CHECK(result.playback_started);
  CHECK(!result.playback_done);
  // 两帧都已写入设备（合成没有等播放），但设备时间没有前进，所以一帧都没播完。
  CHECK(result.pcm_frames.size() == kExpectedFrames);
  CHECK(sink.pcm_samples().size() == kExpectedFrames * kFrameSamples);
  CHECK(playback.played_count() == 0);
  // 未播完不等于“还在排队”：轮次结束时 Session 已经停止播放并丢弃未播部分，
  // 因此待播为 0，而 played_count() 仍为 0 如实反映“没有任何一帧播完”。
  CHECK(playback.pending_count() == 0);
  CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);
  CHECK(result.state == SessionStateMachine::State::kIdle);

  // 未播完不是成功也不是取消：状态机仍按统一收敛路径回到 Idle，但没有 TtsDone。
  const auto transitions = session.state_machine().trace();
  CHECK(transitions.size() == 5);
  CHECK(transitions.back() == "cancelling--cancel_complete-->idle");

  // 交付给播放组件的帧必须与同一文本的确定性合成结果逐采样一致。
  const auto reference = ExpectedFrames(kL1Answer);
  CHECK(reference.size() == result.pcm_frames.size());
  for (std::size_t index = 0; index < reference.size(); ++index) {
    CHECK(reference.at(index).samples == result.pcm_frames.at(index).samples);
  }

  // 同一批音频在“设备时间会前进”的条件下必须能播完：用新的一轮播放重放同样的
  // 帧，逐帧推进设备时间并轮询，最后应当报告完成。它证明上一轮之所以未完成，
  // 原因是设备时间没有前进，而不是音频本身有问题。
  runtime::ManualPlaybackClock advancing_clock;
  backend::FakeAudioSink advancing_sink;
  CHECK(advancing_sink.open().ok());
  runtime::LogicalClockPlayback replay(advancing_sink, advancing_clock);
  CHECK(replay.start().ok());
  for (const auto& frame : result.pcm_frames) {
    CHECK(replay.render(frame).ok());
    advancing_clock.advance(domain::kAudioFrameDurationMs);
    replay.poll();
  }
  CHECK(replay.played_count() == kExpectedFrames);
  CHECK(replay.pending_count() == 0);
  CHECK(advancing_sink.pcm_samples().size() == kExpectedFrames * kFrameSamples);
}

// 控制意图不变量：“停止”必须按统一取消语义收尾，且不得退化成知识问答——本轮
// 不调用检索、不调用 LLM、不合成任何新音频，也不会把停止变成一条回答。
void TestStopControlCancelsWithoutSynthesis() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeAsr asr({"停止"});
  CountingTts tts;
  backend::FakeAudioSink sink;
  CHECK(sink.open().ok());
  runtime::ManualPlaybackClock clock;
  runtime::LogicalClockPlayback playback(sink, clock);
  runtime::SessionRuntime session(asr, rag, router, tts, playback);

  SessionTurnInput input;
  input.stream_id = "mic-l0";
  input.generation = 1;
  input.request_id = "req-l0";
  input.pcm_samples = Samples(kFrameSamples, 3);
  const auto result = session.run(input);

  CHECK(result.route == backend::RagRouteLevel::kL0);
  CHECK(result.decision.control_action == "cancel");
  CHECK(runtime::is_stop_control_action(result.decision.control_action));
  CHECK(result.cancelled);
  CHECK(!result.completed);
  CHECK(result.pcm_frames.empty());
  CHECK(result.text.empty());
  CHECK(sink.pcm_samples().empty());
  CHECK(playback.played_count() == 0);
  CHECK(result.state == SessionStateMachine::State::kIdle);

  // 控制意图在路由阶段就返回：既没有检索命中，也没有任何合成调用，因此“停止”
  // 不可能变成一次普通知识回答。
  CHECK(result.decision.hits.empty());
  CHECK(tts.calls == 0);
  CHECK(tts.last_text.empty());

  const auto markers = session.trace();
  CHECK(markers.size() == 6);
  CHECK(markers.at(0) == ActivityMarker::kGenerationStarted);
  CHECK(markers.at(1) == ActivityMarker::kCancelAccepted);
  CHECK(markers.at(2) == ActivityMarker::kOldOutputBlocked);
  CHECK(markers.at(3) == ActivityMarker::kExecutionExited);
  CHECK(markers.at(4) == ActivityMarker::kPlaybackCleared);
  CHECK(markers.at(5) == ActivityMarker::kTerminalCancelled);
  CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);

  // 状态机同样按统一取消路径收敛：识别文本先经 Routing 得到控制意图，再从 Routing
  // 进入 Cancelling 并回到 Idle，而不是直接跳回 Idle 或误入 Speaking。
  const auto transitions = session.state_machine().trace();
  CHECK(transitions.size() == 4);
  CHECK(transitions.at(0) == "idle--audio_start-->listening");
  CHECK(transitions.at(1) == "listening--asr_final-->routing");
  CHECK(transitions.at(2) == "routing--cancel-->cancelling");
  CHECK(transitions.at(3) == "cancelling--cancel_complete-->idle");
}

// 空输入与无语音不变量：没有样本时不启动任何后端，也不伪造成功；空白识别结果
// 在路由前收敛，避免静音被当成一次检索请求。
void TestEmptyAndSilentInputFailCleanly() {
  {
    auto rag = MakeIndex();
    backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{0.90, 0.50, 3});
    backend::FakeAsr asr({QuestionText()});
    backend::FakeTts tts;
    backend::FakeAudioSink sink;
    CHECK(sink.open().ok());
    runtime::ManualPlaybackClock clock;
    runtime::LogicalClockPlayback playback(sink, clock);
    runtime::SessionRuntime session(asr, rag, router, tts, playback);

    SessionTurnInput empty;
    empty.stream_id = "mic-empty";
    empty.pcm_samples = {};
    const auto result = session.run(empty);
    CHECK(!result.error.ok());
    CHECK(result.error.code == domain::ErrorCode::kInvalidInput);
    CHECK(!result.completed);
    CHECK(!result.input_done);
    CHECK(result.pcm_frames.empty());
    CHECK(sink.pcm_samples().empty());
    CHECK(playback.played_count() == 0);
    CHECK(result.state == SessionStateMachine::State::kIdle);
    // 空输入在送帧前就收敛：没有 AsrFinal 迁移，但必须出现统一取消收敛阶段。
    const auto transitions = session.state_machine().trace();
    CHECK(transitions.size() == 3);
    CHECK(transitions.at(1) == "listening--cancel-->cancelling");
    CHECK(transitions.at(2) == "cancelling--cancel_complete-->idle");
  }
  {
    auto rag = MakeIndex();
    backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{0.90, 0.50, 3});
    backend::FakeAsr asr({"   \t"});
    backend::FakeTts tts;
    backend::FakeAudioSink sink;
    CHECK(sink.open().ok());
    runtime::ManualPlaybackClock clock;
    runtime::LogicalClockPlayback playback(sink, clock);
    runtime::SessionRuntime session(asr, rag, router, tts, playback);

    SessionTurnInput silent;
    silent.stream_id = "mic-silent";
    silent.pcm_samples = Samples(kFrameSamples, 0);
    const auto result = session.run(silent);
    CHECK(!result.error.ok());
    CHECK(result.error.code == domain::ErrorCode::kInvalidInput);
    CHECK(!result.completed);
    // 送帧本身成功，因此输入阶段标记为完成，但文本、路由与播放都没有发生。
    CHECK(result.input_done);
    CHECK(!result.playback_started);
    CHECK(result.text.empty());
    CHECK(result.pcm_frames.empty());
    CHECK(sink.pcm_samples().empty());
  }
}

// 单帧与尾部不变量：320 个样本恰好一帧也必须走完整条链路；341 个样本切成两帧，
// 第二帧只有 21 个有效样本，编排层不截断也不丢弃尾部。第二轮复用常驻 stream，
// 因此“旧输出封锁”必须被显式记录，而不是被静默覆盖。
void TestSingleFrameAndPaddedTailTurns() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeAsr asr({QuestionText()});
  backend::FakeTts tts;
  runtime::ManualPlaybackClock clock;
  AdvancingSink sink(clock);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  runtime::SessionRuntime session(asr, rag, router, tts, playback);

  SessionTurnInput single;
  single.stream_id = "mic-single";
  single.pcm_samples = Samples(kFrameSamples, 11);
  const auto single_result = session.run(single);
  CHECK(single_result.error.ok());
  CHECK(single_result.completed);
  CHECK(single_result.pcm_frames.size() == kExpectedFrames);

  SessionTurnInput tail;
  tail.stream_id = "mic-single";
  tail.pcm_samples = Samples(kFrameSamples + 21, 13);
  const auto tail_result = session.run(tail);
  CHECK(tail_result.error.ok());
  CHECK(tail_result.completed);
  // 两轮都正常收尾：上一轮已经产生终态，因此新代际不需要再记录“旧输出封锁”。
  // 该标记只在上一轮被中途打断（例如取消或未播完）时出现，代表旧输出被显式作废。
  const auto markers = session.trace();
  CHECK(std::count(markers.begin(), markers.end(), ActivityMarker::kOldOutputBlocked) == 0);
  CHECK(std::count(markers.begin(), markers.end(), ActivityMarker::kTerminalSucceeded) == 2);
  CHECK(markers.back() == ActivityMarker::kTerminalSucceeded);
}

// 结束帧不变量：finish_stream() 必须真正以一条零有效样本的结束事件关闭常驻流，
// 而不是只改一个本地标志位。判据是契约夹具接受了该结束事件——夹具对同一 stream 只
// 允许一次结束，因此“重复关闭失败 + 之后的新轮次失败”共同证明结束事件已经生效。
void TestFinishStreamClosesContractStream() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeAsr asr({QuestionText()});
  backend::FakeTts tts;
  runtime::ManualPlaybackClock clock;
  AdvancingSink sink(clock);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  runtime::SessionRuntime session(asr, rag, router, tts, playback);

  SessionTurnInput input;
  input.stream_id = "mic-finish";
  input.pcm_samples = Samples(kFrameSamples, 6);
  const auto first = session.run(input);
  CHECK(first.error.ok());
  CHECK(first.completed);

  // 第一次关闭成功：结束事件被夹具接受；第二次因“同一 stream 只能结束一次”而失败。
  CHECK(session.finish_stream().ok());
  CHECK(!session.finish_stream().ok());

  const auto before = sink.sample_count();
  const auto rejected = session.run(input);
  CHECK(!rejected.error.ok());
  CHECK(!rejected.completed);
  CHECK(rejected.pcm_frames.empty());
  CHECK(sink.sample_count() == before);
}

// 确定性与不残留不变量：同一输入在同一会话的连续两轮都必须正常完成，并交付逐采样
// 一致的 PCM；第二轮不得把第一轮的任何缓冲或输出残留一起播出去。
// 这里刻意使用“设备按实时速率消耗音频”的成功路径：未播完的轮次会按统一取消语义
// 收尾，而取消会封锁当前合成能力，使下一轮无法复用同一 TTS 实例——这本身是正确
// 的收敛行为，但不适合用来验证“重复运行一致”。
void TestRepeatedRunsAreDeterministic() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeAsr asr({QuestionText()});
  backend::FakeTts tts;
  runtime::ManualPlaybackClock clock;
  AdvancingSink sink(clock);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  runtime::SessionRuntime session(asr, rag, router, tts, playback);

  SessionTurnInput input;
  input.stream_id = "mic-repeat";
  input.pcm_samples = Samples(kFrameSamples, 5);
  const auto first = session.run(input);
  const auto first_pcm = first.pcm_frames;
  const auto second = session.run(input);

  CHECK(first.error.ok());
  CHECK(second.error.ok());
  CHECK(first.completed);
  CHECK(second.completed);
  CHECK(first.pcm_frames.size() == kExpectedFrames);
  CHECK(second.pcm_frames.size() == kExpectedFrames);
  for (std::size_t index = 0; index < first_pcm.size(); ++index) {
    CHECK(first_pcm.at(index).samples == second.pcm_frames.at(index).samples);
  }
  // 两轮各写出自己的 2 帧：设备内容正好是两轮之和，没有缺帧，也没有重复补播。
  CHECK(sink.write_count() == 2 * kExpectedFrames);
  CHECK(sink.sample_count() == 2 * kExpectedFrames * kFrameSamples);
}

// 取消可以发生在播放期间的帧边界上：停止受理后不再交付新帧，但已经播出的帧必须
// 保留（不可撤回），未播出的交付被丢弃，本轮只有取消终态。新回答共 2 帧，取消发生
// 在第 1 帧播出之后，因此恰好 1 帧已播出、1 帧从未播出。
void TestCancelDuringPlaybackKeepsPlayedFrames() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeAsr asr({QuestionText()});
  backend::FakeTts tts;
  runtime::ManualPlaybackClock clock;
  SessionSlot slot;
  CancellingSink sink(clock, slot);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  runtime::SessionRuntime session(asr, rag, router, tts, playback);
  slot.session = &session;

  SessionTurnInput input;
  input.stream_id = "mic-cancel";
  input.pcm_samples = Samples(kFrameSamples, 4);
  const auto result = session.run(input);

  CHECK(result.cancelled);
  CHECK(!result.completed);
  CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);
  CHECK(result.state == SessionStateMachine::State::kIdle);
  CHECK(sink.write_count() == 1);
  CHECK(sink.sample_count() == kFrameSamples);
  CHECK(result.pcm_frames.size() == 1);
  CHECK(playback.played_count() == 1);
  CHECK(playback.pending_count() == 0);
  CHECK(result.playback_started);
  CHECK(!result.playback_done);
}

// 取消受理与恢复：Idle 时取消是空操作，不迁移阶段也不改变代际水位；取消清理完成
// 后下一轮必须能重新开始，证明每轮开始时显式解除了上一轮的封锁，且新代际不继承
// 旧一轮的输出。
void TestCancelIsIdempotentAndDoesNotLockSession() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeAsr asr({QuestionText()});
  backend::FakeTts tts;
  runtime::ManualPlaybackClock clock;
  AdvancingSink sink(clock);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  runtime::SessionRuntime session(asr, rag, router, tts, playback);

  const auto idle_generation = session.state_machine().generation();
  CHECK(session.cancel().ok());
  CHECK(session.cancel().ok());
  CHECK(session.state_machine().state() == SessionStateMachine::State::kIdle);
  CHECK(session.state_machine().generation() == idle_generation);

  SessionTurnInput input;
  input.stream_id = "mic-gen";
  input.pcm_samples = Samples(kFrameSamples, 6);
  const auto first = session.run(input);
  CHECK(first.error.ok());
  CHECK(first.completed);
  CHECK(first.terminal_marker == ActivityMarker::kTerminalSucceeded);
  const auto after_first = sink.sample_count();

  // 第二轮沿用同一常驻 stream：序号继续推进、旧一轮输出被显式封锁，且本轮同样
  // 交付完整帧序列，说明取消与轮次结束都没有把会话锁死。
  const auto next = session.run(input);
  CHECK(next.error.ok());
  CHECK(next.completed);
  CHECK(next.pcm_frames.size() == kExpectedFrames);
  CHECK(next.terminal_marker == ActivityMarker::kTerminalSucceeded);
  CHECK(sink.sample_count() == after_first + kExpectedFrames * kFrameSamples);
  // 两轮都正常收尾，因此不需要“旧输出封锁”标记；该标记只在上一轮被中途打断时出现。
  const auto markers = session.trace();
  CHECK(std::count(markers.begin(), markers.end(), ActivityMarker::kOldOutputBlocked) == 0);
  CHECK(std::count(markers.begin(), markers.end(), ActivityMarker::kTerminalSucceeded) == 2);
}

// 播放完成与逻辑时钟一致：交付的帧立即写入设备，但“播完”必须等逻辑时间覆盖其
// 时长。时钟不前进时第一帧虽然已经写进设备，仍未播完；每次推进一个帧时长并轮询，
// 已播完计数才前进，全部交付播完后本轮报告完成。这条用例保护“合成结束 ≠ 播放
// 结束”，且不依赖真实时间。
void TestPlaybackCompletionFollowsLogicalClock() {
  backend::FakeAudioSink sink;
  CHECK(sink.open().ok());
  TestFrozenClock clock;
  runtime::LogicalClockPlayback playback(sink, clock);
  CHECK(playback.start().ok());

  // 同一文本的确定性合成结果即本轮应当播出的 PCM：取前后两帧分别交付。
  const auto frames = ExpectedFrames(kL1Answer);
  CHECK(frames.size() == kExpectedFrames);
  const auto first = frames.front();
  const auto second = frames.back();

  CHECK(playback.render(first).ok());
  CHECK(playback.render(second).ok());
  // 两帧都已写入设备，但逻辑时间仍是 0，因此一帧都还没播完。
  CHECK(sink.frames().size() == kExpectedFrames);
  CHECK(playback.played_count() == 0);
  CHECK(playback.pending_count() == kExpectedFrames);

  // 推进到 20 ms 并轮询：第一帧算作播完，第二帧的门槛是 40 ms。
  clock.advance(domain::kAudioFrameDurationMs);
  playback.poll();
  CHECK(playback.played_count() == 1);
  CHECK(playback.pending_count() == 1);

  // 再推进到 40 ms 并轮询：第二帧播完，缓冲清空，本轮报告完成。
  clock.advance(domain::kAudioFrameDurationMs);
  playback.poll();
  CHECK(playback.played_count() == kExpectedFrames);
  CHECK(playback.pending_count() == 0);
  CHECK(sink.frames().front().samples == first.samples);
  CHECK(sink.frames().back().samples == second.samples);
}

// 输出失败不变量：播放设备在写入时返回错误，本轮必须以该设备错误收敛，不得报成功，
// 也不得把“已交付”当成“已播出”。这条用例覆盖票据要求里的“输出失败”。
void TestPlaybackDeviceFailureFailsTurn() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeAsr asr({QuestionText()});
  backend::FakeTts tts;

  // 每次交付都返回设备错误的播放组件：不写任何设备，也不报告任何帧已播完。
  // 它用来验证会话在没有写进任何音频时不会伪造成功终态。
  class FailingPlayback final : public runtime::IAudioPlayback {
   public:
    domain::OperationResult start() override {
      return domain::OperationResult::success();
    }

    domain::OperationResult render(const domain::AudioFrame&) override {
      ++calls;
      return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                              "播放设备写入失败");
    }

    domain::OperationResult stop() noexcept override {
      return domain::OperationResult::success();
    }

    domain::OperationResult error() const override {
      return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                              "播放设备写入失败");
    }

    std::size_t played_count() const noexcept override {
      return 0;
    }

    std::size_t pending_count() const noexcept override {
      return 0;
    }

    std::vector<domain::AudioFrame> played_frames() const override {
      return {};
    }

    void set_cancelled_flag(const std::atomic<bool>*) noexcept override {}

    std::size_t calls = 0;
  };

  FailingPlayback playback;
  runtime::SessionRuntime session(asr, rag, router, tts, playback);

  SessionTurnInput input;
  input.stream_id = "mic-playback-fail";
  input.pcm_samples = Samples(kFrameSamples, 9);
  const auto result = session.run(input);

  // 直答共 2 帧，两帧都会交付给播放组件（每次回调是一个独立的交付线性化点），
  // 但设备一帧都没写成；本轮不把它改写成成功，终态是取消收敛。
  CHECK(playback.calls == kExpectedFrames);
  CHECK(!result.completed);
  CHECK(!result.error.ok());
  CHECK(result.error.code == domain::ErrorCode::kDeviceFailure);
  CHECK(result.pcm_frames.empty());
  CHECK(result.state == SessionStateMachine::State::kIdle);
  CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);
}

int main() {
  TestCompletedTurnSequenceAndEvidence();
  TestFrozenClockKeepsPlaybackIncomplete();
  TestStopControlCancelsWithoutSynthesis();
  TestEmptyAndSilentInputFailCleanly();
  TestSingleFrameAndPaddedTailTurns();
  TestRepeatedRunsAreDeterministic();
  TestFinishStreamClosesContractStream();
  TestCancelDuringPlaybackKeepsPlayedFrames();
  TestCancelIsIdempotentAndDoesNotLockSession();
  TestPlaybackCompletionFollowsLogicalClock();
  TestPlaybackDeviceFailureFailsTurn();
  return 0;
}

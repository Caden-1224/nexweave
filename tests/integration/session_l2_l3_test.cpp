#include "../test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "fake_asr.hpp"
#include "fake_audio.hpp"
#include "fake_llm.hpp"
#include "fake_rag.hpp"
#include "fake_tts.hpp"
#include "session_runtime.hpp"

using namespace nexweave;
using nexweave::runtime::ActivityMarker;
using nexweave::runtime::SessionRuntime;
using nexweave::runtime::SessionRuntimeConfig;
using nexweave::runtime::SessionStateMachine;
using nexweave::runtime::SessionTurnInput;

namespace {

// 统一音频契约的固定帧长；本文件所有“帧”都指这个长度。
constexpr std::size_t kFrameSamples = domain::kAudioFrameSamples;

// L2 命中文本与用户问题。Fake 检索器的匹配方向是“片段文本包含查询”，因此问题刻意取成
// 命中文本的子串，用例才聚焦编排而不是检索夹具本身。
constexpr char kL2Answer[] = "the capital of france is paris";
constexpr char kL2Question[] = "capital of france";
// L3 没有命中：问题直接进入 LLM。
constexpr char kL3Question[] = "hello there";
constexpr char kL3Answer[] = "hello there friend";

std::vector<std::int16_t> Samples(std::size_t count, std::int16_t value) {
  return std::vector<std::int16_t>(count, value);
}

// 只含一个高分片段的索引；用例通过抬高直答阈值把它变成 L2（命中提供上下文），或换成
// 不含命中的问题得到 L3（无检索上下文）。
backend::FakeRag MakeIndex() {
  std::vector<capability::RetrievedChunk> chunks;
  chunks.push_back(capability::RetrievedChunk{"geo-capital-fr", kL2Answer, 0.97});
  return backend::FakeRag(std::move(chunks));
}

// 把一段回答按固定字节数切成 token 序列，模拟“token 边界与句子边界无关”。
std::vector<std::string> SplitTokens(const std::string& text, std::size_t step) {
  std::vector<std::string> tokens;
  for (std::size_t offset = 0; offset < text.size(); offset += step) {
    tokens.push_back(text.substr(offset, step));
  }
  return tokens;
}

// 独立复算期望 PCM：同一确定性合成实现、同一文本必须得到逐采样一致的帧序列。编排层只
// 负责搬运帧，不允许改写样本、帧数或元数据。
std::vector<domain::AudioFrame> ExpectedFrames(const std::string& text) {
  backend::FakeTts reference;
  std::vector<domain::AudioFrame> frames;
  reference.set_callback([&frames](const domain::AudioFrame& frame) { frames.push_back(frame); });
  CHECK(reference.synthesize(text).ok());
  return frames;
}

// 多段文本的期望帧按顺序拼接：这是“分段合成”的可复算结果，用来证明总输出恰好等于各
// 片段合成结果的有序拼接，既没有重复也没有遗漏。
std::vector<domain::AudioFrame> ConcatenatedFrames(const std::vector<std::string>& texts) {
  std::vector<domain::AudioFrame> frames;
  for (const auto& text : texts) {
    const auto part = ExpectedFrames(text);
    frames.insert(frames.end(), part.begin(), part.end());
  }
  return frames;
}

// 是否出现过某个活动标记；用于断言“取消或失败后没有成功终态”这类否定性事实。
bool HasMarker(const std::vector<ActivityMarker>& markers, ActivityMarker marker) {
  return std::find(markers.begin(), markers.end(), marker) != markers.end();
}

// 记录合成输入的 TTS：包装 FakeTts，额外保存每次送入的文本。用它同时核对“合成了几次”
// 与“每次都送了什么”，避免只验证次数而漏掉片段内容错位。
class RecordingTts final : public capability::ITts {
 public:
  domain::OperationResult set_callback(capability::AudioEventCallback callback) override {
    inner_.set_callback(std::move(callback));
    return domain::OperationResult::success();
  }

  domain::OperationResult synthesize(const std::string& text) override {
    texts.push_back(text);
    return inner_.synthesize(text);
  }

  domain::OperationResult cancel() noexcept override {
    return inner_.cancel();
  }

  std::vector<std::string> texts;

 private:
  backend::FakeTts inner_;
};

// 记录提示词的 LLM：包装 FakeLlm，保存 generate 收到的 prompt，用来核对 L2 注入了检索
// 上下文、L3 只提交用户问题本身。它同时实现进度探针接口，因此内层夹具仍能进入“被会话
// 观测”的路径——否则会话不会给它挂探针，内层回调也就永远不会被注册。
class PromptRecordingLlm final : public capability::ILlm, public capability::IGenerationProbe {
 public:
  explicit PromptRecordingLlm(std::vector<std::string> tokens) : inner_(std::move(tokens)) {}

  domain::OperationResult set_callback(capability::TextEventCallback callback) override {
    callback_ = callback;
    return inner_.set_callback(std::move(callback));
  }

  domain::OperationResult generate(const std::string& prompt) override {
    prompt_ = prompt;
    return inner_.generate(prompt);
  }

  domain::OperationResult cancel() noexcept override {
    return inner_.cancel();
  }

  // 会话通过 dynamic_cast 发现探针接缝后调用本方法，本类把探针转交给内层夹具；内层
  // 夹具在交付 token 时直接通知它，因此顺序证据来自实际执行而不是层层转述。
  void set_progress_probe(capability::IGenerationProbe* probe) noexcept override {
    inner_.set_progress_probe(probe);
  }

  const std::string& prompt() const noexcept {
    return prompt_;
  }

 private:
  backend::FakeLlm inner_;
  capability::TextEventCallback callback_;
  std::string prompt_;
};

// 先报错、再尝试交付 token 的 LLM：验证“错误事件一旦到达就停止后续合成”，因此它必须
// 记录 token 是否被真正交付过。
class ErrorThenTokenLlm final : public capability::ILlm {
 public:
  domain::OperationResult set_callback(capability::TextEventCallback callback) override {
    if (!callback) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    callback_ = std::move(callback);
    return domain::OperationResult::success();
  }

  domain::OperationResult generate(const std::string& prompt) override {
    if (!callback_ || prompt.empty()) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    callback_({capability::TextEventKind::kError, "",
               domain::Error{domain::ErrorCode::kBackendFailure, "夹具注入的生成失败"}});
    callback_({capability::TextEventKind::kToken, kL2Answer, {}});
    callback_({capability::TextEventKind::kDone, "", {}});
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    return domain::OperationResult::success();
  }

 private:
  capability::TextEventCallback callback_;
};

// 直接投放指定 token 的 LLM：用来构造 FakeLlm 拒绝的输入（例如把多字节字符切成两半），
// 使“非法 UTF-8 尾部”这一条失败分支可以被独立触发。它不校验 token 内容，因为那正是本
// 夹具要注入的病态输入。
class ScriptedTokenLlm final : public capability::ILlm {
 public:
  explicit ScriptedTokenLlm(std::vector<std::string> tokens) : tokens_(std::move(tokens)) {}

  domain::OperationResult set_callback(capability::TextEventCallback callback) override {
    if (!callback) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    callback_ = std::move(callback);
    return domain::OperationResult::success();
  }

  domain::OperationResult generate(const std::string& prompt) override {
    if (!callback_ || prompt.empty()) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    for (const auto& token : tokens_) {
      callback_({capability::TextEventKind::kToken, token, {}});
    }
    callback_({capability::TextEventKind::kDone, "", {}});
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    return domain::OperationResult::success();
  }

 private:
  std::vector<std::string> tokens_;
  capability::TextEventCallback callback_;
};

// 在第 N 次写入时开始失败的设备：让“合成进行到一半时播放设备报错”这条路径可复现。
// 已经成功写入的帧保留在设备里，用于核对失败轮次不会把已播内容从证据里抹掉。
class FailingSink final : public capability::IAudioSink {
 public:
  explicit FailingSink(std::size_t fail_from_write) : fail_from_(fail_from_write) {}

  domain::OperationResult open() override {
    return domain::OperationResult::success();
  }

  domain::OperationResult write(const domain::AudioFrame& frame) override {
    ++writes_;
    if (writes_ >= fail_from_) {
      return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                              "夹具注入的写入失败");
    }
    samples_.insert(samples_.end(), frame.samples.begin(), frame.samples.end());
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

 private:
  std::size_t fail_from_ = 1;
  std::size_t writes_ = 0;
  std::vector<std::int16_t> samples_;
};

// 不产生任何帧的 TTS：只统计被调用次数，用于证明“未注入 LLM 的 L2/L3 轮次根本不会开始
// 合成”，而不必读取会话私有状态。
class SilentTts final : public capability::ITts {
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

// 不前进的逻辑时钟：用于复现“合成已结束但播放尚未结束”以及“播放缓冲填满”。
class FrozenClock final : public runtime::IPlaybackClock {
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

// 按实时速率消耗音频的设备汇：每写出一帧就推进一个帧时长的逻辑时间，因此一次 run() 内
// 可以走完“合成 → 播放 → 完成”。真实声卡就是这个语义，用它避免依赖墙钟。
class AdvancingSink : public capability::IAudioSink {
 public:
  explicit AdvancingSink(runtime::IPlaybackClock& clock) : clock_(clock) {}

  domain::OperationResult open() override {
    return domain::OperationResult::success();
  }

  domain::OperationResult write(const domain::AudioFrame& frame) override {
    samples_.insert(samples_.end(), frame.samples.begin(), frame.samples.end());
    ++writes_;
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

 private:
  runtime::IPlaybackClock& clock_;
  std::size_t writes_ = 0;
  std::vector<std::int16_t> samples_;
};

// 生成进度探针：把“生成尚未结束”变成可断言的事件。它记录两个关键位置上已经提交的活动
// 标记数量，从而不依赖墙钟、睡眠或调度顺序地证明“首段播放早于生成结束”。
//
// 它同时实现观察者与探针两个角色：测试通常在夹具之后才构造探针，因此不能用构造函数
// 注入；而会话要在生成开始前把自己挂上。两个角色共享同一份记录逻辑，见 RecordToken。
class TraceProbe final : public capability::IGenerationObserver, public capability::IGenerationProbe {
 public:
  explicit TraceProbe(const SessionRuntime& session) : session_(session) {}

  void on_token_delivered(const std::string& /*token*/) override {
    RecordToken();
  }

  void on_generation_completed() override {
    completion_trace = session_.trace().size();
  }

  void on_generation_failed(const std::string& /*message*/) override {
    failure_reported = true;
  }

  bool failed() const noexcept override {
    return failure_reported;
  }

  std::size_t tokens = 0;
  bool first_delivery_seen = false;
  bool failure_reported = false;
  std::size_t first_delivery_trace = 0;
  std::size_t completion_trace = 0;

 private:
  // 首次交付完成时记录已提交的标记数量。此刻“播放开始”可能已经提交——本记录的语义是
  // “首次交付完成时文本定稿还没有提交”，而不是“播放尚未开始”。
  void RecordToken() {
    ++tokens;
    if (!first_delivery_seen) {
      first_delivery_seen = true;
      first_delivery_trace = session_.trace().size();
    }
  }

  const SessionRuntime& session_;
};

// 在第 N 次写出时请求取消的设备，模拟“用户在新回答刚开始播报时喊停止”。会话指针延迟
// 绑定，避免“设备必须先于会话构造、又要回调会话”这一循环依赖。
struct SessionSlot {
  SessionRuntime* session = nullptr;
};

class CancellingSink final : public AdvancingSink {
 public:
  CancellingSink(runtime::IPlaybackClock& clock, SessionSlot& slot, std::size_t cancel_at)
      : AdvancingSink(clock), slot_(slot), cancel_at_(cancel_at) {}

  domain::OperationResult write(const domain::AudioFrame& frame) override {
    const auto written = AdvancingSink::write(frame);
    if (write_count() == cancel_at_ && slot_.session != nullptr) {
      // 第 N 帧已经播出：它属于已播放声音，不可撤回；从下一帧起会话不再交付。
      CHECK(slot_.session->cancel().ok());
    }
    return written;
  }

 private:
  SessionSlot& slot_;
  std::size_t cancel_at_ = 1;
};

// 一轮 L2/L3 用例的输入：常驻流身份与一帧已采集音频。样本取值不影响本文件断言。
SessionTurnInput TurnInput(const char* stream_id, const char* request_id) {
  SessionTurnInput input;
  input.stream_id = stream_id;
  input.request_id = request_id;
  input.generation = 1;
  input.pcm_samples = Samples(kFrameSamples, 7);
  return input;
}

// 重叠执行不变量：L2 生成过程未结束时首段音频已经交给播放组件，且首段播放严格早于
// “文本定稿”。断言使用生成探针的事件先后关系，而不是墙钟或“最终同时存在文本与 PCM”。
void TestL2OverlapAndEvidence() {
  const std::string answer = kL2Answer;
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  backend::FakeAsr asr({kL2Question});
  // 用 4 字节一个 token 切开回答，制造“token 边界与句子边界无关”的输入；容量上限
  // 12 字节保证生成过程中就会切出第一个片段，而不是等全部 token 到齐。
  const auto tokens = SplitTokens(answer, 4);
  backend::FakeLlm llm(tokens);
  RecordingTts tts;
  runtime::ManualPlaybackClock clock;
  AdvancingSink sink(clock);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntimeConfig config;
  config.text_chunk_max_bytes = 12;
  SessionRuntime session(asr, rag, router, tts, playback, &llm, config);
  TraceProbe probe(session);
  // 夹具先于探针构造，因此观察者在轮次开始前挂接；会话随后在生成开始前把自己挂成探针。
  llm.set_observer(&probe);

  const auto result = session.run(TurnInput("mic-l2-ok", "req-l2-ok"));

  CHECK(result.error.ok());
  CHECK(result.completed);
  CHECK(!result.cancelled);
  CHECK(result.input_done);
  CHECK(result.playback_started);
  CHECK(result.playback_done);
  CHECK(result.route == backend::RagRouteLevel::kL2);
  CHECK(result.request_id == "req-l2-ok");
  CHECK(result.state == SessionStateMachine::State::kIdle);
  CHECK(result.terminal_marker == ActivityMarker::kTerminalSucceeded);
  CHECK(result.text == answer);
  CHECK(!result.backpressure);
  CHECK(result.decision.hits.size() == 1);
  CHECK(result.decision.hits.front().id == "geo-capital-fr");

  // 探针证据：首次 token 交付时文本定稿尚未提交，生成结束时它才出现。因此“首段播放早于
  // 文本定稿”是编排顺序的结果，而不是巧合。
  CHECK(probe.tokens == tokens.size());
  CHECK(probe.first_delivery_seen);
  CHECK(probe.first_delivery_trace < probe.completion_trace);

  const auto markers = session.trace();
  const auto playback_started =
      std::find(markers.begin(), markers.end(), ActivityMarker::kPlaybackStarted);
  const auto generation_done =
      std::find(markers.begin(), markers.end(), ActivityMarker::kGenerationDone);
  CHECK(playback_started != markers.end());
  CHECK(generation_done != markers.end());
  CHECK(playback_started < generation_done);
  CHECK(markers.size() == 6);
  CHECK(markers.at(0) == ActivityMarker::kGenerationStarted);
  CHECK(markers.at(1) == ActivityMarker::kPlaybackStarted);
  CHECK(markers.at(2) == ActivityMarker::kGenerationDone);
  CHECK(markers.at(3) == ActivityMarker::kSynthesisDone);
  CHECK(markers.at(4) == ActivityMarker::kPlaybackDone);
  CHECK(markers.at(5) == ActivityMarker::kTerminalSucceeded);

  // 状态机轨迹必须出现 Thinking：L2 与 L1 的区别正是“先经过 LLM 再进入 Speaking”。
  const auto transitions = session.state_machine().trace();
  CHECK(transitions.size() == 5);
  CHECK(transitions.at(0) == "idle--audio_start-->listening");
  CHECK(transitions.at(1) == "listening--asr_final-->routing");
  CHECK(transitions.at(2) == "routing--route_l2_l3-->thinking");
  CHECK(transitions.at(3) == "thinking--llm_done-->speaking");
  CHECK(transitions.at(4) == "speaking--tts_done-->idle");

  // 分句结果就是合成输入：片段有序、按上限切分、末尾尾段被刷新。
  CHECK(tts.texts.size() == 3);
  CHECK(tts.texts.at(0).size() == 12);
  CHECK(tts.texts.at(1).size() == 12);
  CHECK(tts.texts.at(2) == answer.substr(24));
  std::string joined;
  for (const auto& text : tts.texts) {
    joined += text;
  }
  CHECK(joined == answer);

  // 总输出等于各片段合成结果的有序拼接，且设备写入量与帧数一致。
  const auto expected = ConcatenatedFrames(tts.texts);
  CHECK(result.pcm_frames.size() == expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    CHECK(result.pcm_frames.at(index).samples == expected.at(index).samples);
  }
  CHECK(playback.played_count() == expected.size());
  CHECK(playback.pending_count() == 0);
  CHECK(sink.write_count() == expected.size());
  CHECK(sink.sample_count() == expected.size() * kFrameSamples);
  // 该设备每写出一帧就按实时速率记为播完，因此“写出的帧”在交付当时就已经播完：积压峰值
  // 为 0，说明生成没有跑到播放前面。有积压的场景由背压用例覆盖。
  CHECK(result.peak_pending_frames == 0);
}

// 分句与重叠的多句场景：回答含句末标点时必须按句切分并逐句合成，同时保证“首句播放”
// 出现在“文本定稿”之前；片段顺序与内容必须与原文一致，不能重复也不能遗漏。
void TestMultiSentenceChunkingKeepsOrder() {
  std::vector<capability::RetrievedChunk> chunks;
  chunks.push_back(capability::RetrievedChunk{"greet", "你好世界。第二句话在这里", 0.97});
  backend::FakeRag rag(std::move(chunks));
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  backend::FakeAsr asr({"你好世界"});
  const std::vector<std::string> tokens = {"你好世界。", "第二句话在这里"};
  backend::FakeLlm llm(tokens);
  RecordingTts tts;
  runtime::ManualPlaybackClock clock;
  AdvancingSink sink(clock);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntime session(asr, rag, router, tts, playback, &llm);
  TraceProbe probe(session);
  // 夹具先于探针构造，因此观察者在轮次开始前挂接；会话随后在生成开始前把自己挂成探针。
  llm.set_observer(&probe);

  const auto result = session.run(TurnInput("mic-l2-sentences", "req-l2-sentences"));

  CHECK(result.error.ok());
  CHECK(result.completed);
  CHECK(result.route == backend::RagRouteLevel::kL2);
  CHECK(result.text == "你好世界。第二句话在这里");
  // 整个回答的音频在生成结束之前就已经合成并播出完毕：生成结束时已提交的标记里已经含有
  // “播放开始”，而文本定稿（kGenerationDone）在最终轨迹里排在其后。
  CHECK(probe.tokens == tokens.size());
  CHECK(probe.first_delivery_seen);
  CHECK(probe.first_delivery_trace == probe.completion_trace);

  CHECK(tts.texts.size() == 2);
  CHECK(tts.texts.at(0) == "你好世界。");
  CHECK(tts.texts.at(1) == "第二句话在这里");
  const auto expected = ConcatenatedFrames(tts.texts);
  CHECK(result.pcm_frames.size() == expected.size());
  CHECK(result.playback_done);

  const auto markers = session.trace();
  const auto playback_started =
      std::find(markers.begin(), markers.end(), ActivityMarker::kPlaybackStarted);
  const auto generation_done =
      std::find(markers.begin(), markers.end(), ActivityMarker::kGenerationDone);
  CHECK(playback_started != markers.end());
  CHECK(generation_done != markers.end());
  CHECK(playback_started < generation_done);
  CHECK(probe.completion_trace < markers.size());
}

// 提示词不变量：L2 必须携带检索上下文且按版本化版式拼装，因此回答的依据可以直接复算；
// L3 没有可用命中时不注入任何未经支持的上下文，只提交用户问题本身。
void TestPromptContainsContextOnlyForL2() {
  {
    auto rag = MakeIndex();
    backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
    backend::FakeAsr asr({kL2Question});
    PromptRecordingLlm llm({kL2Answer});
    backend::FakeTts tts;
    runtime::ManualPlaybackClock clock;
    AdvancingSink sink(clock);
    CHECK(sink.open().ok());
    runtime::LogicalClockPlayback playback(sink, clock);
    SessionRuntime session(asr, rag, router, tts, playback, &llm);

    const auto result = session.run(TurnInput("mic-prompt-l2", "req-prompt-l2"));
    CHECK(result.error.ok());
    CHECK(result.route == backend::RagRouteLevel::kL2);
    const auto& prompt = llm.prompt();
    CHECK(prompt.find("nexweave-l2-v1") != std::string::npos);
    CHECK(prompt.find("context:") != std::string::npos);
    CHECK(prompt.find(kL2Answer) != std::string::npos);
    CHECK(prompt.find(std::string("question: ") + kL2Question) != std::string::npos);
  }
  {
    // 问题与索引不匹配：路由得到 L3，prompt 必须等于用户问题本身。
    auto rag = MakeIndex();
    backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
    backend::FakeAsr asr({kL3Question});
    PromptRecordingLlm llm({kL3Answer});
    backend::FakeTts tts;
    runtime::ManualPlaybackClock clock;
    AdvancingSink sink(clock);
    CHECK(sink.open().ok());
    runtime::LogicalClockPlayback playback(sink, clock);
    SessionRuntime session(asr, rag, router, tts, playback, &llm);

    const auto result = session.run(TurnInput("mic-prompt-l3", "req-prompt-l3"));
    CHECK(result.error.ok());
    CHECK(result.route == backend::RagRouteLevel::kL3);
    CHECK(result.decision.hits.empty());
    CHECK(llm.prompt() == kL3Question);
    CHECK(result.text == kL3Answer);
    CHECK(result.completed);
  }
}

// 有界背压不变量：播放缓冲达到声明容量时本轮明确失败并立即停止继续合成，而不是在模型
// 回调里无界等待，也不是静默丢帧。已经写出设备的帧如实保留在结果里供证据核对。
void TestPlaybackBackpressureFailsTurnInBoundedWay() {
  std::vector<capability::RetrievedChunk> chunks;
  chunks.push_back(capability::RetrievedChunk{"long", "第一句很长很长很长。第二句更长一些", 0.97});
  backend::FakeRag rag(std::move(chunks));
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  backend::FakeAsr asr({"第一句"});
  backend::FakeLlm llm({"第一句很长很长很长。", "第二句更长一些"});
  RecordingTts tts;
  FrozenClock clock;
  // 设备不推进时钟：写出的帧会一直停留在“尚未播完”，从而可靠地把缓冲填到上限。
  backend::FakeAudioSink sink;
  CHECK(sink.open().ok());
  // 冻结时钟让已写出的帧一直停留在“尚未播完”，从而在容量为 2 时可靠触发背压。
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntimeConfig config;
  config.playback_queue_capacity = 2;
  SessionRuntime session(asr, rag, router, tts, playback, &llm, config);

  const auto result = session.run(TurnInput("mic-l2-backpressure", "req-l2-backpressure"));

  CHECK(!result.error.ok());
  CHECK(result.error.code == domain::ErrorCode::kBackendFailure);
  CHECK(result.backpressure);
  CHECK(!result.completed);
  CHECK(!result.playback_done);
  CHECK(result.peak_pending_frames == 2);
  // 第一句的两帧已经写出设备，属于“确实交付过”的输出，必须保留在证据里。
  CHECK(result.pcm_frames.size() == 2);
  CHECK(sink.pcm_samples().size() == 2 * kFrameSamples);
  CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);
  CHECK(result.state == SessionStateMachine::State::kIdle);
  CHECK(!HasMarker(session.trace(), ActivityMarker::kTerminalSucceeded));

  // 失败路径同样收敛到 Idle，并留下“封锁旧输出”的阶段，而不是从 Thinking 直接跳回。
  const auto transitions = session.state_machine().trace();
  CHECK(!transitions.empty());
  CHECK(transitions.back() == "cancelling--cancel_complete-->idle");
  CHECK(std::find(transitions.begin(), transitions.end(), "thinking--cancel-->cancelling") !=
        transitions.end());
}

// 取消不变量：播报期间受理停止后本轮只有取消终态，不再交付新帧，也不留下成功终态；
// 已写出的帧不可撤回，仍作为“已经播出的声音”保留在证据里。
void TestCancelDuringPlayback() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  backend::FakeAsr asr({kL2Question});
  backend::FakeLlm llm(SplitTokens(kL2Answer, 6));
  RecordingTts tts;
  runtime::ManualPlaybackClock clock;
  SessionSlot slot;
  CancellingSink sink(clock, slot, 1);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntimeConfig config;
  config.text_chunk_max_bytes = 12;
  SessionRuntime session(asr, rag, router, tts, playback, &llm, config);
  slot.session = &session;

  const auto result = session.run(TurnInput("mic-l2-cancel", "req-l2-cancel"));

  CHECK(result.cancelled);
  CHECK(!result.completed);
  CHECK(!result.playback_done);
  // 取消来源是设备回调里的停止指令，不是新语音打断：两者都以取消收敛，但必须可区分。
  CHECK(!result.interrupted_by_speech);
  CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);
  CHECK(result.state == SessionStateMachine::State::kIdle);
  // 第 1 帧已经播出：它保留在设备与证据里；之后不再有新帧。
  CHECK(!result.pcm_frames.empty());
  CHECK(sink.write_count() == 1);

  const auto markers = session.trace();
  CHECK(HasMarker(markers, ActivityMarker::kCancelAccepted));
  CHECK(HasMarker(markers, ActivityMarker::kOldOutputBlocked));
  CHECK(HasMarker(markers, ActivityMarker::kPlaybackCleared));
  CHECK(!HasMarker(markers, ActivityMarker::kTerminalSucceeded));
}

// 空生成与缺失依赖不变量：LLM 没有产生可合成文本时明确失败、不报告成功；未注入 LLM 时
// L2/L3 同样明确拒绝，且根本不开始合成，而不是伪造一条回答或退化成直答。
void TestEmptyGenerationAndMissingLlmFailCleanly() {
  {
    auto rag = MakeIndex();
    backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
    backend::FakeAsr asr({kL2Question});
    // 全空白 token 能成功注册并交付，但分句后没有任何可合成文本。
    backend::FakeLlm llm({"   "});
    RecordingTts tts;
    runtime::ManualPlaybackClock clock;
    AdvancingSink sink(clock);
    CHECK(sink.open().ok());
    runtime::LogicalClockPlayback playback(sink, clock);
    SessionRuntime session(asr, rag, router, tts, playback, &llm);

    const auto result = session.run(TurnInput("mic-l2-empty", "req-l2-empty"));
    CHECK(!result.error.ok());
    CHECK(result.route == backend::RagRouteLevel::kL2);
    CHECK(!result.completed);
    CHECK(result.pcm_frames.empty());
    CHECK(tts.texts.empty());
    CHECK(sink.sample_count() == 0);
    CHECK(result.state == SessionStateMachine::State::kIdle);
    CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);
  }
  {
    auto rag = MakeIndex();
    backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
    backend::FakeAsr asr({kL2Question});
    SilentTts tts;
    runtime::ManualPlaybackClock clock;
    AdvancingSink sink(clock);
    CHECK(sink.open().ok());
    runtime::LogicalClockPlayback playback(sink, clock);
    // 不注入 LLM：L2/L3 必须明确失败，而不是退化成直答或空回答。
    SessionRuntime session(asr, rag, router, tts, playback);

    const auto result = session.run(TurnInput("mic-l2-no-llm", "req-l2-no-llm"));
    CHECK(!result.error.ok());
    CHECK(result.error.code == domain::ErrorCode::kBackendFailure);
    CHECK(result.route == backend::RagRouteLevel::kL2);
    CHECK(!result.completed);
    CHECK(result.text.empty());
    CHECK(tts.calls == 0);
    CHECK(result.state == SessionStateMachine::State::kIdle);
  }
  {
    auto rag = MakeIndex();
    backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
    backend::FakeAsr asr({kL2Question});
    backend::FakeLlm llm({kL2Answer});
    SilentTts tts;
    runtime::ManualPlaybackClock clock;
    AdvancingSink sink(clock);
    CHECK(sink.open().ok());
    runtime::LogicalClockPlayback playback(sink, clock);
    // 分句上限为 0 会让“待合成文本有明确容量”这条不变量失效，因此生成路径必须拒绝该
    // 配置，而不是接受一次没有上界的合成请求。
    SessionRuntimeConfig config;
    config.text_chunk_max_bytes = 0;
    SessionRuntime session(asr, rag, router, tts, playback, &llm, config);

    const auto result = session.run(TurnInput("mic-l2-no-limit", "req-l2-no-limit"));
    CHECK(!result.error.ok());
    CHECK(result.error.code == domain::ErrorCode::kInvalidInput);
    CHECK(result.route == backend::RagRouteLevel::kL2);
    CHECK(!result.completed);
    CHECK(result.text.empty());
    CHECK(tts.calls == 0);
    // 配置在进入 Thinking 之前就被拒绝：轨迹里没有 route_l2_l3 迁移，也没有任何合成。
    const auto transitions = session.state_machine().trace();
    CHECK(std::find(transitions.begin(), transitions.end(), "routing--route_l2_l3-->thinking") ==
          transitions.end());
    CHECK(result.state == SessionStateMachine::State::kIdle);
  }
}

// 重复运行不变量：同一会话连续两轮必须各自完整、互不残留——上一轮的文本、片段与错误
// 不能出现在下一轮；相同输入得到逐采样一致的结果，证明实现是确定性的。
void TestRepeatedRunsAreDeterministicWithoutResidue() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  backend::FakeAsr asr({kL2Question});
  backend::FakeLlm llm(SplitTokens(kL2Answer, 4));
  RecordingTts tts;
  runtime::ManualPlaybackClock clock;
  AdvancingSink sink(clock);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntimeConfig config;
  config.text_chunk_max_bytes = 12;
  SessionRuntime session(asr, rag, router, tts, playback, &llm, config);

  const auto first = session.run(TurnInput("mic-l2-repeat", "req-l2-repeat-1"));
  const auto second = session.run(TurnInput("mic-l2-repeat", "req-l2-repeat-2"));

  CHECK(first.error.ok());
  CHECK(second.error.ok());
  CHECK(first.completed);
  CHECK(second.completed);
  CHECK(first.request_id == "req-l2-repeat-1");
  CHECK(second.request_id == "req-l2-repeat-2");
  // 两轮回答完全相同且各自完整：既没有把上一轮的文本拼接进来，也没有丢帧。
  CHECK(first.text == kL2Answer);
  CHECK(second.text == kL2Answer);
  CHECK(first.pcm_frames.size() == second.pcm_frames.size());
  for (std::size_t index = 0; index < first.pcm_frames.size(); ++index) {
    CHECK(first.pcm_frames.at(index).samples == second.pcm_frames.at(index).samples);
  }
  // 每轮各合成一次完整回答（分成同样多的片段），因此片段总数是单轮的两倍，且内容重复。
  CHECK(tts.texts.size() == 6);
  CHECK(tts.texts.at(0) == tts.texts.at(3));
  CHECK(tts.texts.at(1) == tts.texts.at(4));
  CHECK(tts.texts.at(2) == tts.texts.at(5));
  const auto markers = session.trace();
  CHECK(std::count(markers.begin(), markers.end(), ActivityMarker::kTerminalSucceeded) == 2);
}

// 文本错误不变量：LLM 通过错误事件报告失败时本轮以该错误收敛，错误之后交付的 token 不再
// 进入合成，因此不会出现“失败轮次仍在产生音频”的部分成功。
void TestLlmErrorEventStopsSynthesisAndFailsTurn() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  backend::FakeAsr asr({kL2Question});
  ErrorThenTokenLlm llm;
  RecordingTts tts;
  runtime::ManualPlaybackClock clock;
  AdvancingSink sink(clock);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntime session(asr, rag, router, tts, playback, &llm);

  const auto result = session.run(TurnInput("mic-l2-error", "req-l2-error"));

  CHECK(!result.error.ok());
  CHECK(result.error.code == domain::ErrorCode::kBackendFailure);
  CHECK(!result.completed);
  CHECK(result.text.empty());
  CHECK(tts.texts.empty());
  CHECK(result.pcm_frames.empty());
  CHECK(sink.sample_count() == 0);
  CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);
  CHECK(!HasMarker(session.trace(), ActivityMarker::kTerminalSucceeded));
}

// 非法 UTF-8 不变量：生成只交付了半个多字节字符时本轮必须明确失败，并且绝不把这一段
// 送去合成——半个字符既无法发音，也会让“回答内容”与“生成内容”悄悄不一致。
void TestTruncatedUtf8TailFailsWithoutSynthesis() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  backend::FakeAsr asr({kL2Question});
  const std::string han = "好";  // E5 A5 BD
  ScriptedTokenLlm llm({std::string("你好"), han.substr(0, 1)});
  RecordingTts tts;
  runtime::ManualPlaybackClock clock;
  AdvancingSink sink(clock);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntime session(asr, rag, router, tts, playback, &llm);

  const auto result = session.run(TurnInput("mic-l2-utf8", "req-l2-utf8"));

  CHECK(!result.error.ok());
  CHECK(result.error.code == domain::ErrorCode::kInvalidInput);
  CHECK(!result.completed);
  // “你好”整句留在缓冲里没有交付（它后面跟着半个字符，flush 因此不会刷新），所以本轮
  // 一帧音频都没有产生：宁可不回答，也不把截断文本合成出来。
  CHECK(tts.texts.empty());
  CHECK(result.pcm_frames.empty());
  CHECK(sink.sample_count() == 0);
  CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);
  CHECK(!HasMarker(session.trace(), ActivityMarker::kTerminalSucceeded));
}

// 中途设备失败不变量：合成进行到一半时播放设备报错，本轮以设备错误收敛，已经写出的帧
// 仍然作为“确实交付过”的证据保留；失败轮次不得继续把后续片段塞进已经报错的设备。
void TestPlaybackDeviceFailureFailsTurnWithoutResidue() {
  auto rag = MakeIndex();
  backend::FakeRagRouter router(rag, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  backend::FakeAsr asr({kL2Question});
  const std::vector<std::string> tokens = {"你好世界。", "第二句话在这里"};
  backend::FakeLlm llm(tokens);
  RecordingTts tts;
  runtime::ManualPlaybackClock clock;
  // 第二帧写入开始失败：第一句已经播出，后续合成会被停止。
  FailingSink sink(2);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntime session(asr, rag, router, tts, playback, &llm);

  const auto result = session.run(TurnInput("mic-l2-device", "req-l2-device"));

  CHECK(!result.error.ok());
  CHECK(result.error.code == domain::ErrorCode::kDeviceFailure);
  CHECK(!result.completed);
  CHECK(!result.playback_done);
  // 设备只接受了第 1 帧，因此证据里只有这一帧，且它确实写进了设备。
  CHECK(sink.sample_count() == kFrameSamples);
  CHECK(result.pcm_frames.size() <= 1);
  CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);
  CHECK(!HasMarker(session.trace(), ActivityMarker::kTerminalSucceeded));
}

}  // namespace

int main() {
  TestL2OverlapAndEvidence();
  TestMultiSentenceChunkingKeepsOrder();
  TestPromptContainsContextOnlyForL2();
  TestPlaybackBackpressureFailsTurnInBoundedWay();
  TestCancelDuringPlayback();
  TestEmptyGenerationAndMissingLlmFailCleanly();
  TestRepeatedRunsAreDeterministicWithoutResidue();
  TestLlmErrorEventStopsSynthesisAndFailsTurn();
  TestTruncatedUtf8TailFailsWithoutSynthesis();
  TestPlaybackDeviceFailureFailsTurnWithoutResidue();
  return 0;
}

// Session 端到端取消集成夹具。
//
// 职责：把“取消”当作一条跨阶段的统一收敛路径来验收，而不是只验收某个阶段的局部行为。
// 覆盖 Listening、Routing 边界、Thinking、Speaking，以及生成与播放同时活跃时的取消；
// 同时验收旧输出的封锁、后台执行的退出、待播数据的清空和下一轮的恢复。
//
// 保护的不变量（每个用例在断言旁注明它保护哪一条）：
//   1. 取消受理后本轮不再启动新工作：Listening 停止送帧并跳过检索/生成/合成，已经算出但
//      尚未提交的路由决策也不再提交。
//   2. 取消是“取消”，不是“失败”：被取消的后端随后返回的 kCancelled 或错误只是取消的结果，
//      不改变终态，也不写进 result.error。
//   3. 旧代际的迟到 partial/final/token/PCM/done/error 一律不得提交：既不影响本轮终态，
//      也不出现在下一轮。
//   4. 待播数据在取消时被清空，已经写出的声音保留（不可撤回）。
//   5. 重复取消与“最后一帧才受理取消”的竞态都只产生一个终态。
//   6. 打断只作废旧回答的输出，不消耗新语音：新语音仍能作为下一轮的完整输入。
//
// 夹具全部是确定性的、无线程、无睡眠的：取消由能力回调或设备写入回调在确定的事件边界上
// 触发，因此结论不依赖墙钟或调度顺序。本文件不引入 NPU、声卡、网络或文件依赖。
#include "../test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

// 统一音频契约的固定帧长与帧时长：1 帧 = 320 样本 = 20 ms。本文件所有容量与时间都以帧为单位。
constexpr std::size_t kFrameSamples = domain::kAudioFrameSamples;
constexpr std::size_t kFrameMs = domain::kAudioFrameDurationMs;

// 直答命中文本同时就是 L1 的回答；L2 用例把阈值抬到 1.01 之上，使同一命中只提供上下文。
constexpr char kHitText[] = "the capital of france is paris";
// 两个问句都必须是命中文本的子串（确定性检索器的匹配方向是“片段文本包含查询”），
// 差别只在阈值：前者 ≥ 0.90 得到 L1，后者落在 [0.50, 0.90) 得到 L2。
constexpr char kL1Question[] = "the capital of france";
constexpr char kL2Question[] = "capital of france";

std::vector<std::int16_t> Samples(std::size_t count, std::int16_t value) {
  return std::vector<std::int16_t>(count, value);
}

// 生成夹具用的定长重复文本：长度决定切出的片段数与每段帧数，内容本身不参与断言。
std::string Repeat(char character, std::size_t count) {
  return std::string(count, character);
}

// 是否出现过某个活动标记；用于断言“取消或失败后没有成功终态”这类否定性事实。
bool HasMarker(const std::vector<ActivityMarker>& markers, ActivityMarker marker) {
  return std::find(markers.begin(), markers.end(), marker) != markers.end();
}

// 是否出现过某条状态机迁移；用于断言“根本没有进入过某个阶段”。
bool HasTransition(const std::vector<std::string>& transitions, const std::string& transition) {
  return std::find(transitions.begin(), transitions.end(), transition) != transitions.end();
}

// 会话指针的延迟绑定：播放设备必须先于会话构造（会话借用设备），而设备又要回调会话，
// 因此先构造设备、会话构造完成后再回填指针。未回填时设备只记录、不触发取消。
struct SessionSlot {
  SessionRuntime* session = nullptr;
};

// 一次轮次的输入：常驻流身份、请求标识与一段已采集音频。样本取值不参与本文件断言。
SessionTurnInput TurnInput(const char* stream_id, const char* request_id, std::size_t frames) {
  SessionTurnInput input;
  input.stream_id = stream_id;
  input.request_id = request_id;
  input.generation = 1;
  input.pcm_samples = Samples(frames * kFrameSamples, 7);
  return input;
}

// 记录检索次数的检索夹具：包装确定性索引，只增加一次调用计数。
// 它存在的理由是把“本轮到底有没有走到路由”变成可断言的事实——取消若在 Listening 阶段
// 就被受理，检索调用次数必须是 0，而不是靠“结果文本为空”间接推断。
class RecordingRetriever final : public capability::IRag {
 public:
  explicit RecordingRetriever(std::vector<capability::RetrievedChunk> chunks)
      : inner_(std::move(chunks)) {}

  domain::Result<std::vector<capability::RetrievedChunk>> retrieve(const std::string& query,
                                                                   std::size_t top_k) override {
    ++calls;
    return inner_.retrieve(query, top_k);
  }

  std::size_t calls = 0;

 private:
  backend::FakeRag inner_;
};

// 只含一个高分片段的索引，供 L1/L2 两条分支共用。
RecordingRetriever MakeIndex() {
  std::vector<capability::RetrievedChunk> chunks;
  chunks.push_back(capability::RetrievedChunk{"geo-capital-fr", kHitText, 0.97});
  return RecordingRetriever(std::move(chunks));
}

// 可脚本化的识别夹具：按帧发布 partial，在指定的第 N 次送帧之后请求会话取消，并紧接着
// 交付一次“迟到的 final”，模拟取消受理后识别仍然报告了一次结果。
// 会话侧必须忽略这次迟到结果（不路由、不合成），因此它是“旧识别结果不污染新请求”的
// 直接证据来源。armed 只在第一次触发后关闭，使同一实例能在后续轮次里正常恢复。
class ScriptedAsr final : public capability::IAsr {
 public:
  ScriptedAsr(std::string text, SessionSlot& slot, std::size_t cancel_at_feed)
      : text_(std::move(text)), slot_(slot), cancel_at_feed_(cancel_at_feed) {}

  domain::OperationResult set_callback(capability::TextEventCallback callback) override {
    if (!callback) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    callback_ = std::move(callback);
    // 成功注册就是显式的新轮次边界：清游标与取消封锁，但**不**重新武装取消触发，
    // 否则恢复用例里的第二轮会被同一个脚本再取消一次，无法验证“取消不锁死会话”。
    feeds_ = 0;
    cancelled_ = false;
    return domain::OperationResult::success();
  }

  domain::OperationResult feed(const domain::AudioFrame& /*frame*/, bool is_last) override {
    if (cancelled_) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
    }
    if (!callback_) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput, "未注册回调");
    }
    ++feeds_;
    callback_({is_last ? capability::TextEventKind::kFinal : capability::TextEventKind::kPartial,
               text_, {}});
    if (armed_ && feeds_ == cancel_at_feed_ && slot_.session != nullptr) {
      armed_ = false;
      (void)slot_.session->cancel();
      // 取消受理之后仍交付一次 partial 与一次 final：两者都必须被会话隔离，绝不能进入路由。
      // partial 顺带证明“中间结果不会累积成回答文本”，final 则证明“最终结果也不会”。
      ++late_partials_;
      callback_({capability::TextEventKind::kPartial, text_, {}});
      ++late_finals_;
      callback_({capability::TextEventKind::kFinal, text_, {}});
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    ++cancel_calls_;
    cancelled_ = true;
    return domain::OperationResult::success();
  }

  std::size_t feeds() const noexcept {
    return feeds_;
  }
  std::size_t cancel_calls() const noexcept {
    return cancel_calls_;
  }
  std::size_t late_finals() const noexcept {
    return late_finals_;
  }
  std::size_t late_partials() const noexcept {
    return late_partials_;
  }

 private:
  std::string text_;
  SessionSlot& slot_;
  std::size_t cancel_at_feed_ = 0;
  capability::TextEventCallback callback_;
  std::size_t feeds_ = 0;
  std::size_t cancel_calls_ = 0;
  std::size_t late_finals_ = 0;
  std::size_t late_partials_ = 0;
  bool armed_ = true;
  bool cancelled_ = false;
};

// 可协作取消的生成夹具：严格实现“每个 token 交付前检查取消封锁”，因此交付数就是
// “后端实际算到哪一步”的可观察事实；同时可在交付第 N 个 token 之后请求会话取消，
// 用来把取消精确地钉在生成阶段（Thinking）的中间，而不是等生成结束。
class CooperativeLlm final : public capability::ILlm {
 public:
  CooperativeLlm(std::vector<std::string> tokens, SessionSlot& slot, std::size_t cancel_after_token)
      : tokens_(std::move(tokens)), slot_(slot), cancel_after_token_(cancel_after_token) {}

  domain::OperationResult set_callback(capability::TextEventCallback callback) override {
    if (!callback) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    callback_ = std::move(callback);
    delivered_ = 0;
    cancel_calls_ = 0;
    cancelled_.store(false);
    return domain::OperationResult::success();
  }

  domain::OperationResult generate(const std::string& prompt) override {
    if (cancelled_.load()) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled, "本轮已被取消");
    }
    if (!callback_ || prompt.empty()) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    for (const auto& token : tokens_) {
      // 交付前的检查就是后端侧的取消线性化点：会话在回调内传播过来的 cancel() 从下一个
      // token 起生效，已经交付的那一个 token 无法撤回（并发窗口，不做虚假承诺）。
      if (cancelled_.load()) {
        return domain::OperationResult::failure(domain::ErrorCode::kCancelled, "生成被取消");
      }
      callback_({capability::TextEventKind::kToken, token, {}});
      ++delivered_;
      if (cancel_after_token_ != 0 && !cancel_fired_ && delivered_ == cancel_after_token_ &&
          slot_.session != nullptr) {
        cancel_fired_ = true;
        (void)slot_.session->cancel();
      }
    }
    callback_({capability::TextEventKind::kDone, "", {}});
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    ++cancel_calls_;
    cancelled_.store(true);
    return domain::OperationResult::success();
  }

  std::size_t delivered() const noexcept {
    return delivered_;
  }
  std::size_t cancel_calls() const noexcept {
    return cancel_calls_;
  }

 private:
  std::vector<std::string> tokens_;
  SessionSlot& slot_;
  std::size_t cancel_after_token_ = 0;
  capability::TextEventCallback callback_;
  std::size_t delivered_ = 0;
  std::size_t cancel_calls_ = 0;
  bool cancel_fired_ = false;
  std::atomic<bool> cancelled_{false};
};

// 无法即时中断的生成夹具：记录 cancel() 但**故意不生效**，继续交付剩余 token，并在最后
// 追加一个错误事件与 done 后正常返回。它模拟“SDK 不支持中途打断”的真实边界，用来验证
// 隔离点在会话侧而不在后端：旧代际的迟到 token、error 与 done 都必须被丢弃。
class StubbornLlm final : public capability::ILlm {
 public:
  StubbornLlm(std::vector<std::string> tokens, SessionSlot& slot, std::size_t cancel_after_token)
      : tokens_(std::move(tokens)), slot_(slot), cancel_after_token_(cancel_after_token) {}

  domain::OperationResult set_callback(capability::TextEventCallback callback) override {
    if (!callback) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    callback_ = std::move(callback);
    delivered_ = 0;
    cancel_calls_ = 0;
    return domain::OperationResult::success();
  }

  domain::OperationResult generate(const std::string& prompt) override {
    if (!callback_ || prompt.empty()) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    for (const auto& token : tokens_) {
      // 刻意不检查取消：本夹具的职责就是把“取消之后仍然到达的旧结果”真的送出来。
      callback_({capability::TextEventKind::kToken, token, {}});
      ++delivered_;
      if (cancel_after_token_ != 0 && !cancel_fired_ && delivered_ == cancel_after_token_ &&
          slot_.session != nullptr) {
        cancel_fired_ = true;
        (void)slot_.session->cancel();
      }
    }
    // 迟到事件：连同错误一起送出，验证“旧 error 既不改变本轮终态、也不污染下一轮”。
    // 只有“本轮确实被取消过”时才补发错误——否则下一轮会收到一个真正的错误事件，
    // 那样测的就不是隔离性，而是夹具自身的行为。
    if (cancel_calls_ > 0) {
      callback_({capability::TextEventKind::kError, "",
                 domain::Error{domain::ErrorCode::kBackendFailure, "夹具注入的迟到错误"}});
    }
    callback_({capability::TextEventKind::kDone, "", {}});
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    ++cancel_calls_;
    return domain::OperationResult::success();
  }

  std::size_t delivered() const noexcept {
    return delivered_;
  }
  std::size_t cancel_calls() const noexcept {
    return cancel_calls_;
  }

 private:
  std::vector<std::string> tokens_;
  SessionSlot& slot_;
  std::size_t cancel_after_token_ = 0;
  capability::TextEventCallback callback_;
  std::size_t delivered_ = 0;
  std::size_t cancel_calls_ = 0;
  bool cancel_fired_ = false;
};

// 记录合成输入的合成夹具：把 cancel() 如实转发给确定性合成内核，因此“合成被取消”表现为
// synthesize 返回 kCancelled 且不再交付后续帧；同时保存每次送入的文本，用来核对
// “取消之后不再启动新的合成”。
class RecordingTts final : public capability::ITts {
 public:
  domain::OperationResult set_callback(capability::AudioEventCallback callback) override {
    return inner_.set_callback(std::move(callback));
  }

  domain::OperationResult synthesize(const std::string& text) override {
    texts.push_back(text);
    return inner_.synthesize(text);
  }

  domain::OperationResult cancel() noexcept override {
    ++cancel_calls;
    return inner_.cancel();
  }

  std::vector<std::string> texts;
  std::size_t cancel_calls = 0;

 private:
  backend::FakeTts inner_;
};

// 无法即时中断的合成夹具：cancel() 只记录、不生效，synthesize 会把整段文本的帧全部交付。
// 它让“取消之后的迟到 PCM”真实存在，从而能验证会话在交付边界上把它们挡在播放组件之外。
class StubbornTts final : public capability::ITts {
 public:
  domain::OperationResult set_callback(capability::AudioEventCallback callback) override {
    return inner_.set_callback(std::move(callback));
  }

  domain::OperationResult synthesize(const std::string& text) override {
    ++calls;
    // 内层夹具的取消封锁从未被置位，因此本夹具会一直把帧交付完。
    return inner_.synthesize(text);
  }

  domain::OperationResult cancel() noexcept override {
    ++cancel_calls;
    return domain::OperationResult::success();
  }

  std::size_t calls = 0;
  std::size_t cancel_calls = 0;

 private:
  backend::FakeTts inner_;
};

// 逻辑时钟：只由测试显式推进，不读墙钟，因此“播出多少”完全由用例决定。
class ManualClock final : public runtime::IPlaybackClock {
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

// 计数播放设备：只统计写入次数与样本数，不落盘、不创建资源。
// advance_clock 为真时每写入一帧就推进一个帧时长的逻辑时间，复现真实声卡“写进去就开始播”
// 的语义；为假时冻结时间，让已写出的帧停留在“已写未播完”，从而可靠地制造并观察待播队列。
class CountingSink : public capability::IAudioSink {
 public:
  CountingSink(runtime::IPlaybackClock& clock, bool advance_clock)
      : clock_(clock), advance_clock_(advance_clock) {}

  domain::OperationResult open() override {
    return domain::OperationResult::success();
  }

  domain::OperationResult write(const domain::AudioFrame& frame) override {
    ++writes_;
    samples_ += frame.samples.size();
    if (advance_clock_) {
      clock_.advance(static_cast<std::int64_t>(kFrameMs));
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
    return samples_;
  }

 protected:
  runtime::IPlaybackClock& clock_;
  bool advance_clock_ = false;
  std::size_t writes_ = 0;
  std::size_t samples_ = 0;
};

// 在第 N 次写入时请求取消的设备：把“用户在播报过程中喊停”固定在一个确定的帧边界上，
// 使取消的线性化点可复现。cancel_times>1 时连续请求多次，用来验证重复取消不会产生第二个
// 终态；两次调用的返回值都被保存，便于核对“只有第一次改变状态”。
class CancellingSink final : public CountingSink {
 public:
  CancellingSink(runtime::IPlaybackClock& clock, bool advance_clock, SessionSlot& slot,
                 std::size_t cancel_at, std::size_t cancel_times = 1)
      : CountingSink(clock, advance_clock),
        slot_(slot),
        cancel_at_(cancel_at),
        cancel_times_(cancel_times) {}

  domain::OperationResult write(const domain::AudioFrame& frame) override {
    const auto written = CountingSink::write(frame);
    if (!fired_ && cancel_at_ != 0 && writes_ == cancel_at_ && slot_.session != nullptr) {
      fired_ = true;
      for (std::size_t attempt = 0; attempt < cancel_times_; ++attempt) {
        cancel_results.push_back(slot_.session->cancel());
      }
    }
    return written;
  }

  std::vector<domain::OperationResult> cancel_results;

 private:
  SessionSlot& slot_;
  std::size_t cancel_at_ = 0;
  std::size_t cancel_times_ = 1;
  bool fired_ = false;
};

// 停止即失败的播放组件：包装真实播放组件，只把 stop() 换成设备错误，其余调用原样转发。
// 它验证“清理失败”是一个必须被报告的事实：取消受理了、终态是取消，但“待播数据已经被丢弃”
// 并不成立，此时把响应当成已经静默就是虚假承诺。包装而不是另写一个替身，是为了让
// “失败发生在停止这一步”成为唯一变量。
class FailingStopPlayback final : public runtime::IAudioPlayback {
 public:
  explicit FailingStopPlayback(runtime::IAudioPlayback& inner) : inner_(inner) {}

  domain::OperationResult start() override {
    return inner_.start();
  }

  domain::OperationResult render(const domain::AudioFrame& frame) override {
    return inner_.render(frame);
  }

  domain::OperationResult stop() noexcept override {
    ++stop_calls;
    return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                            "夹具注入的停止失败");
  }

  void poll() override {
    inner_.poll();
  }

  domain::OperationResult error() const override {
    return inner_.error();
  }

  std::size_t pending_count() const noexcept override {
    return inner_.pending_count();
  }

  std::size_t played_count() const noexcept override {
    return inner_.played_count();
  }

  std::vector<domain::AudioFrame> played_frames() const override {
    return inner_.played_frames();
  }

  void set_cancelled_flag(const std::atomic<bool>* flag) noexcept override {
    inner_.set_cancelled_flag(flag);
  }

  std::size_t stop_calls = 0;

 private:
  runtime::IAudioPlayback& inner_;
};

// 打断监视夹具：持有一条待消费的起音通知，以及一份“新语音”的音频。
// 会话只能取走通知，取不走音频——这正是接缝的语义；测试据此证明旧回答的取消清理
// 没有碰过新语音，且新语音仍能作为下一轮的完整输入进入识别。
class BargeInMonitorFixture final : public runtime::IBargeInMonitor {
 public:
  void Arm(const runtime::SpeechStartNotice& notice) {
    notice_ = notice;
    armed_ = true;
  }

  std::optional<runtime::SpeechStartNotice> take_speech_started() override {
    if (!armed_) {
      return std::nullopt;
    }
    armed_ = false;
    return notice_;
  }

  std::vector<domain::AudioFrame> pending_speech;
  std::size_t take_calls = 0;

 private:
  runtime::SpeechStartNotice notice_;
  bool armed_ = false;
};

// 在指定写入次数上向打断监视器投递一条起音通知的设备：把“用户开始说话”钉在确定的
// 交付边界上。它只武装通知，不触发会话取消——打断必须由会话在自己的交付边界上消费。
class ArmingSink final : public CountingSink {
 public:
  ArmingSink(runtime::IPlaybackClock& clock, bool advance_clock, BargeInMonitorFixture& monitor,
             std::size_t arm_at, runtime::SpeechStartNotice notice)
      : CountingSink(clock, advance_clock),
        monitor_(monitor),
        arm_at_(arm_at),
        notice_(std::move(notice)) {}

  domain::OperationResult write(const domain::AudioFrame& frame) override {
    const auto written = CountingSink::write(frame);
    if (!fired_ && arm_at_ != 0 && writes_ == arm_at_) {
      fired_ = true;
      monitor_.Arm(notice_);
    }
    return written;
  }

 private:
  BargeInMonitorFixture& monitor_;
  std::size_t arm_at_ = 0;
  runtime::SpeechStartNotice notice_;
  bool fired_ = false;
};

// 取消轨迹的四段事实必须齐全，且只能有一个取消终态、不能有成功终态。它把“受理、封锁旧输出、
// 执行退出、播放清理”四条独立事实一次校验完；具体的因果证据（后端是否真的停了、待播是否真的
// 清空）由各用例分别给出，不在这里用一句“取消成功”代替——夹具的 cancel() 是无条件记账的，
// 单靠标记顺序证明不了任何下游动作真的发生过。
void CheckCancelTrace(const SessionRuntime& session) {
  const auto markers = session.trace();
  CHECK(HasMarker(markers, ActivityMarker::kCancelAccepted));
  CHECK(HasMarker(markers, ActivityMarker::kOldOutputBlocked));
  CHECK(HasMarker(markers, ActivityMarker::kExecutionExited));
  CHECK(HasMarker(markers, ActivityMarker::kPlaybackCleared));
  CHECK(std::count(markers.begin(), markers.end(), ActivityMarker::kTerminalCancelled) == 1);
  CHECK(std::count(markers.begin(), markers.end(), ActivityMarker::kTerminalSucceeded) == 0);
}

// Listening 阶段取消不变量：取消在送帧中途被受理时，会话必须停止继续送帧、在回调之外
// 通知识别退出，并直接收敛——不提交 AsrFinal 迁移、不检索、不生成、不合成。
// 它同时保护“旧识别结果不污染本轮”：识别在取消之后交付的迟到 final 既不能触发路由，
// 也不能变成回答文本。
void TestCancelWhileListeningStopsFeedingAndSkipsRouting() {
  auto retriever = MakeIndex();
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  SessionSlot slot;
  // 4 帧输入，在第 2 帧之后请求取消：此时后两帧尚未送入识别。
  ScriptedAsr asr(kL1Question, slot, 2);
  RecordingTts tts;
  ManualClock clock;
  CountingSink sink(clock, /*advance_clock=*/true);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntime session(asr, retriever, router, tts, playback);
  slot.session = &session;

  const auto result = session.run(TurnInput("mic-listen-cancel", "req-listen-cancel", 4));

  // 终态：取消而不是失败。result.error 保持成功，这是“取消不伪装成错误”的直接判据。
  CHECK(result.cancelled);
  CHECK(!result.completed);
  CHECK(result.error.ok());
  CHECK(result.state == SessionStateMachine::State::kIdle);
  CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);
  // 后两帧没有送入识别，因此输入阶段没有被标记为完成。
  CHECK(!result.input_done);
  CHECK(asr.feeds() == 2);
  // 识别被显式通知退出本轮；调用发生在送帧回调之外，符合 IAsr 的重入约束。
  CHECK(asr.cancel_calls() == 1);
  // 识别确实交付过一次迟到的 partial 与 final，但会话完全没有使用它们。
  CHECK(asr.late_finals() == 1);
  CHECK(asr.late_partials() == 1);
  CHECK(result.text.empty());
  // 没有走到路由，也没有产生任何音频：检索调用次数为 0 是“跳过路由”的直接证据。
  CHECK(retriever.calls == 0);
  CHECK(tts.texts.empty());
  CHECK(result.pcm_frames.empty());
  CHECK(sink.write_count() == 0);
  CheckCancelTrace(session);

  // 状态机轨迹必须停在 Listening → Cancelling → Idle，绝不出现 asr_final 与路由迁移。
  const auto transitions = session.state_machine().trace();
  CHECK(transitions.size() == 3);
  CHECK(transitions.at(0) == "idle--audio_start-->listening");
  CHECK(transitions.at(1) == "listening--cancel-->cancelling");
  CHECK(transitions.at(2) == "cancelling--cancel_complete-->idle");
  CHECK(!HasTransition(transitions, "listening--asr_final-->routing"));
  // 取消与开启新代际各推进一次水位，因此本轮之后水位为 2。
  CHECK(session.state_machine().generation() == 2);

  // 恢复：同一个会话的下一轮必须能正常走完整条链路，且迟到 final 不会残留在新回答里。
  const auto recovered = session.run(TurnInput("mic-listen-cancel", "req-listen-recovered", 4));
  CHECK(recovered.error.ok());
  CHECK(recovered.completed);
  CHECK(recovered.text == kHitText);
  CHECK(recovered.route == backend::RagRouteLevel::kL1);
  CHECK(retriever.calls == 1);
  CHECK(sink.write_count() == 2);
  CHECK(session.state_machine().generation() == 3);
}

// Routing 边界取消不变量：取消恰好由最后一次送帧里的识别 final 回调请求时，输入其实已经
// 全部提交，但本轮仍不得提交任何路由决策。路由阶段本身是同步值计算、内部没有回调，所以
// 唯一可达的受理点就是这条边界；这条用例把它固定下来，避免“取消只在 Listening 生效”。
void TestCancelAtFinalBoundarySkipsRoutingAndGeneration() {
  auto retriever = MakeIndex();
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  SessionSlot slot;
  // 4 帧输入，在第 4 帧（也是带 is_last 的那一帧）交付 final 之后请求取消。
  ScriptedAsr asr(kL1Question, slot, 4);
  RecordingTts tts;
  ManualClock clock;
  CountingSink sink(clock, /*advance_clock=*/true);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntime session(asr, retriever, router, tts, playback);
  slot.session = &session;

  const auto result = session.run(TurnInput("mic-final-cancel", "req-final-cancel", 4));

  CHECK(result.cancelled);
  CHECK(!result.completed);
  CHECK(result.error.ok());
  // 全部帧都送入了识别，所以输入阶段本身是完成的；被作废的是它导出的路由与回答。
  CHECK(result.input_done);
  CHECK(asr.feeds() == 4);
  CHECK(asr.late_finals() == 1);
  CHECK(result.text.empty());
  CHECK(retriever.calls == 0);
  CHECK(tts.texts.empty());
  CHECK(sink.write_count() == 0);
  CheckCancelTrace(session);

  const auto transitions = session.state_machine().trace();
  CHECK(transitions.size() == 3);
  CHECK(transitions.at(1) == "listening--cancel-->cancelling");
  CHECK(!HasTransition(transitions, "listening--asr_final-->routing"));
  CHECK(!HasTransition(transitions, "routing--route_l0_l1-->speaking"));
}

// Thinking 阶段取消不变量：生成尚未结束时受理取消，必须（1）把停止传播给正在执行的生成器，
// 使它从下一个 token 起停止计算；（2）已交付的部分文本只能作为证据保留，绝不提交“文本定稿”；
// （3）不再启动新的合成。取消造成的 kCancelled 是取消的结果，不能被写成后端错误。
void TestCancelDuringThinkingStopsGenerationAndKeepsOldTokensOut() {
  auto retriever = MakeIndex();
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  SessionSlot slot;
  // 4 字节一个 token、片段上限 12 字节：每 3 个 token 切出一个片段并立即合成。
  const std::vector<std::string> tokens = {"the ", "capi", "tal ", "of f", "ranc", "e is", " par", "is"};
  CooperativeLlm llm(tokens, slot, /*cancel_after_token=*/3);
  ScriptedAsr asr(kL2Question, slot, 0);
  RecordingTts tts;
  ManualClock clock;
  CountingSink sink(clock, /*advance_clock=*/true);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntimeConfig config;
  config.text_chunk_max_bytes = 12;
  SessionRuntime session(asr, retriever, router, tts, playback, &llm, config);
  slot.session = &session;

  const auto result = session.run(TurnInput("mic-thinking-cancel", "req-thinking-cancel", 2));

  CHECK(result.cancelled);
  CHECK(!result.completed);
  CHECK(!result.playback_done);
  // 取消不是失败：生成器返回的 kCancelled 是取消的结果，不能变成本轮的错误码。
  CHECK(result.error.ok());
  CHECK(result.state == SessionStateMachine::State::kIdle);
  // 生成在最靠近取消点的地方停下：第 3 个 token 之后会话已经把它取消，第 4 个 token 没有交付。
  CHECK(llm.cancel_calls() == 1);
  CHECK(llm.delivered() == 3);
  CHECK(llm.delivered() < tokens.size());
  // 已经交付的部分文本如实保留为证据，但“文本定稿”从未提交，因此它不是一次回答。
  CHECK(result.text == "the capital ");
  CHECK(tts.texts.size() == 1);
  // 取消发生在两次合成之间（生成器在交付第 3 个 token 之后才请求取消），因此当时没有
  // 正在执行的合成调用需要中断；后续片段由“启动前检查取消”直接拒绝，一次都没有开始。
  CHECK(tts.cancel_calls == 0);
  CHECK(sink.write_count() == 1);
  CheckCancelTrace(session);
  const auto markers = session.trace();
  CHECK(!HasMarker(markers, ActivityMarker::kGenerationDone));
  CHECK(!HasMarker(markers, ActivityMarker::kSynthesisDone));
  // 取消必须显式经过 Cancelling，而不是从 Thinking 直接跳回 Idle。
  const auto transitions = session.state_machine().trace();
  CHECK(HasTransition(transitions, "routing--route_l2_l3-->thinking"));
  CHECK(HasTransition(transitions, "thinking--cancel-->cancelling"));
  CHECK(transitions.back() == "cancelling--cancel_complete-->idle");

  // 恢复：同一会话的下一轮必须重放完整回答，且不继承上一轮的部分文本与取消封锁。
  const auto recovered = session.run(TurnInput("mic-thinking-cancel", "req-thinking-recovered", 2));
  CHECK(recovered.error.ok());
  CHECK(recovered.completed);
  CHECK(recovered.text == kHitText);
  CHECK(recovered.pcm_frames.size() == 3);
  CHECK(llm.cancel_calls() == 0);
  CHECK(session.state_machine().generation() == 3);
}

// Speaking 阶段取消与待播清空不变量：合成仍在继续、播放组件里已经排着尚未播完的帧时受理
// 取消，必须（1）停止继续合成；（2）清空尚未播完的排队帧；（3）保留已经写出的声音。
// 时钟刻意冻结，使“已写出但未播完”的积压真实存在，否则这条断言会被“反正已经播完了”掩盖。
void TestCancelDuringSpeakingClearsQueuedPlayback() {
  auto retriever = MakeIndex();
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  SessionSlot slot;
  // 12 字节一个 token、片段上限 12 字节：每个 token 切出一个 12 字节片段，合成 1 帧。
  const std::vector<std::string> tokens = {Repeat('a', 12), Repeat('b', 12), Repeat('c', 12),
                                           Repeat('d', 12), Repeat('e', 12)};
  CooperativeLlm llm(tokens, slot, /*cancel_after_token=*/0);
  ScriptedAsr asr(kL2Question, slot, 0);
  RecordingTts tts;
  ManualClock clock;
  // 时钟冻结：写出的帧一直停留在“已写未播完”，积压因此可观测。
  CancellingSink sink(clock, /*advance_clock=*/false, slot, /*cancel_at=*/3);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntimeConfig config;
  config.text_chunk_max_bytes = 12;
  SessionRuntime session(asr, retriever, router, tts, playback, &llm, config);
  slot.session = &session;

  const auto result = session.run(TurnInput("mic-speaking-cancel", "req-speaking-cancel", 2));

  CHECK(result.cancelled);
  CHECK(!result.completed);
  CHECK(result.error.ok());
  CHECK(result.playback_started);
  CHECK(!result.playback_done);
  // 第 3 帧写出之后受理取消：此后不再有新帧交付，也不再启动新的合成。
  CHECK(sink.write_count() == 3);
  CHECK(tts.texts.size() == 3);
  CHECK(tts.cancel_calls == 1);
  CHECK(llm.cancel_calls() == 1);
  CHECK(llm.delivered() == 3);
  // 已经写出的声音不可撤回，仍然作为“确实交付过”的证据保留。
  CHECK(result.pcm_frames.size() == 3);
  CHECK(sink.sample_count() == 3 * kFrameSamples);
  // 待播队列被清空；冻结时钟下没有任何一帧播完，因此这不是“播完了所以待播为 0”。
  CHECK(playback.pending_count() == 0);
  CHECK(playback.played_count() == 0);
  // 峰值记录的是“生成最多领先播放多少帧”，等于取消时刻的积压量。
  CHECK(result.peak_pending_frames == 3);
  CheckCancelTrace(session);

  // 恢复：推进逻辑时钟让下一轮的帧可以正常播完，验证取消没有把会话或播放组件锁死。
  clock.advance(static_cast<std::int64_t>(kFrameMs * 8));
  const auto recovered = session.run(TurnInput("mic-speaking-cancel", "req-speaking-recovered", 2));
  CHECK(recovered.error.ok());
  CHECK(recovered.completed);
  CHECK(recovered.playback_done);
  CHECK(recovered.pcm_frames.size() == 5);
  CHECK(sink.write_count() == 8);
}

// 迟到结果隔离不变量：后端无法即时中断时，取消之后仍会陆续到达旧代际的 token、PCM、error
// 与 done。会话必须在提交点把它们全部挡住：既不写进播放设备，也不改变本轮终态（尤其是
// 那个迟到的 error 不能把一次正常取消变成后端失败），更不能出现在下一轮。
void TestLateResultsFromUninterruptibleBackendsAreIsolated() {
  auto retriever = MakeIndex();
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  SessionSlot slot;
  // 60 字节一个 token、片段上限 60 字节：每个 token 切出一个 60 字节片段，合成 4 帧。
  // 因此取消发生在第一帧之后时，同一段合成里还会有 3 帧“迟到 PCM”到达会话。
  const std::vector<std::string> tokens = {Repeat('a', 60), Repeat('b', 60), Repeat('c', 60),
                                           Repeat('d', 60), Repeat('e', 60)};
  const std::string whole_answer = tokens.at(0) + tokens.at(1) + tokens.at(2) + tokens.at(3) +
                                   tokens.at(4);
  StubbornLlm llm(tokens, slot, /*cancel_after_token=*/0);
  ScriptedAsr asr(kL2Question, slot, 0);
  StubbornTts tts;
  ManualClock clock;
  CancellingSink sink(clock, /*advance_clock=*/true, slot, /*cancel_at=*/1);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntimeConfig config;
  config.text_chunk_max_bytes = 60;
  SessionRuntime session(asr, retriever, router, tts, playback, &llm, config);
  slot.session = &session;

  const auto result = session.run(TurnInput("mic-late-cancel", "req-late-cancel", 2));

  CHECK(result.cancelled);
  CHECK(!result.completed);
  // 关键判据：迟到错误事件不得改变终态。取消就是取消，不是后端失败。
  CHECK(result.error.ok());
  CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);
  // 会话确实尝试过停止后端，只是后端不配合。
  CHECK(llm.cancel_calls() == 1);
  CHECK(tts.cancel_calls == 1);
  // 后端交付了全部 5 个 token（无法中断），但会话只在取消前合成了一段。
  CHECK(llm.delivered() == tokens.size());
  CHECK(tts.calls == 1);
  // 迟到 PCM 被挡在播放组件之外：设备只收到取消前的那一帧。
  CHECK(sink.write_count() == 1);
  CHECK(sink.sample_count() == kFrameSamples);
  // 迟到的 token 没有进入回答文本，因此 result.text 只包含取消前生成的那一段。
  CHECK(result.text == Repeat('a', 60));
  CHECK(result.pcm_frames.size() == 1);
  CheckCancelTrace(session);
  const auto markers = session.trace();
  CHECK(!HasMarker(markers, ActivityMarker::kGenerationDone));

  // 恢复：下一轮必须完整、干净，既没有上一轮的迟到文本，也没有上一轮的错误。
  const auto recovered = session.run(TurnInput("mic-late-cancel", "req-late-recovered", 2));
  CHECK(recovered.error.ok());
  CHECK(recovered.completed);
  CHECK(recovered.text == whole_answer);
  // 5 个片段各 4 帧 = 20 帧，加取消前写出的那 1 帧，正好是设备的全部写入。
  CHECK(recovered.pcm_frames.size() == 20);
  CHECK(sink.write_count() == 21);
  CHECK(recovered.terminal_marker == ActivityMarker::kTerminalSucceeded);
}

// 重复取消与“最后一帧才受理取消”的竞态不变量：两种情形都只能产生一个终态。
// 重复取消不改变终态、不追加标记；最后一帧受理取消时全部音频都已经播出，但本轮仍然不是
// 成功——打断作废的是这次回答的收尾，而不是它的音频。
void TestRepeatedCancelAndLastFrameRaceHaveSingleTerminal() {
  {
    // 连续请求两次取消：第一次受理，第二次只报告“已经在取消中”，不产生第二个终态。
    auto retriever = MakeIndex();
    backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
    SessionSlot slot;
    ScriptedAsr asr(kL1Question, slot, 0);
    RecordingTts tts;
    ManualClock clock;
    CancellingSink sink(clock, /*advance_clock=*/true, slot, /*cancel_at=*/1, /*cancel_times=*/2);
    CHECK(sink.open().ok());
    runtime::LogicalClockPlayback playback(sink, clock);
    SessionRuntime session(asr, retriever, router, tts, playback);
    slot.session = &session;

    const auto result = session.run(TurnInput("mic-repeat-cancel", "req-repeat-cancel", 2));

    CHECK(sink.cancel_results.size() == 2);
    CHECK(sink.cancel_results.at(0).ok());
    // 第二次取消没有可迁移的阶段：明确失败，而不是伪装成又取消了一次。
    CHECK(!sink.cancel_results.at(1).ok());
    CHECK(sink.cancel_results.at(1).error.code == domain::ErrorCode::kInvalidInput);
    CHECK(result.cancelled);
    CHECK(result.error.ok());
    CheckCancelTrace(session);
    CHECK(session.state_machine().generation() == 2);
  }
  {
    // 在最后一帧的交付边界上受理取消：两帧都已播出，本轮依然只有取消终态。
    auto retriever = MakeIndex();
    backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
    SessionSlot slot;
    ScriptedAsr asr(kL1Question, slot, 0);
    RecordingTts tts;
    ManualClock clock;
    CancellingSink sink(clock, /*advance_clock=*/true, slot, /*cancel_at=*/2);
    CHECK(sink.open().ok());
    runtime::LogicalClockPlayback playback(sink, clock);
    SessionRuntime session(asr, retriever, router, tts, playback);
    slot.session = &session;

    const auto result = session.run(TurnInput("mic-last-cancel", "req-last-cancel", 2));

    CHECK(result.cancelled);
    CHECK(!result.completed);
    CHECK(result.error.ok());
    // 直答共 2 帧：两帧都已写出并播完，但“完成”需要收尾，而收尾已经被取消作废。
    CHECK(sink.write_count() == 2);
    CHECK(playback.played_count() == 2);
    CHECK(!result.playback_done);
    CheckCancelTrace(session);

    // 收尾之后再取消是空操作：状态机已经回到 Idle，两次调用都成功返回，既不迁移状态、
    // 不推进代际，也不追加任何标记，因此本轮已经提交的取消终态不受影响（只有一个）。
    const auto markers_before = session.trace().size();
    const auto generation_before = session.state_machine().generation();
    CHECK(session.cancel().ok());
    CHECK(session.cancel().ok());
    CHECK(session.state_machine().state() == SessionStateMachine::State::kIdle);
    CHECK(session.state_machine().generation() == generation_before);
    CHECK(session.trace().size() == markers_before);
    CheckCancelTrace(session);
  }
}

// 打断不变量：播报期间出现新语音时，会话在同一交付边界上走统一取消路径，并（1）把停止
// 传播给正在执行的生成器与合成器；（2）只消费通知、不取走音频，因此新语音完整保留，
// 旧回答的清理不会清除新轮次的数据；（3）下一轮能拿这段新语音跑出完整回答。
void TestBargeInDuringGenerationStopsBackendsAndKeepsNewSpeech() {
  auto retriever = MakeIndex();
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  SessionSlot slot;
  const std::vector<std::string> tokens = {"the ", "capi", "tal ", "of f", "ranc", "e is", " par", "is"};
  CooperativeLlm llm(tokens, slot, /*cancel_after_token=*/0);
  ScriptedAsr asr(kL2Question, slot, 0);
  RecordingTts tts;
  BargeInMonitorFixture monitor;
  // 新语音：3 帧，取值与旧回答无关，用来核对它作为下一轮输入时逐帧送达。
  for (std::size_t index = 0; index < 3; ++index) {
    const auto frame = domain::AudioFrame::from_samples(Samples(kFrameSamples, 900));
    CHECK(frame.ok());
    monitor.pending_speech.push_back(frame.value);
  }
  runtime::SpeechStartNotice notice;
  notice.stream_id = "mic-barge-in";
  notice.segment_id = 2;
  notice.start_sequence = 17;
  notice.speech_sequence = 20;
  ManualClock clock;
  ArmingSink sink(clock, /*advance_clock=*/true, monitor, /*arm_at=*/1, notice);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  SessionRuntimeConfig config;
  config.text_chunk_max_bytes = 12;
  SessionRuntime session(asr, retriever, router, tts, playback, &llm, config);
  slot.session = &session;
  session.set_barge_in_monitor(&monitor);

  const auto result = session.run(TurnInput("mic-barge-in", "req-barge-in", 2));

  CHECK(result.cancelled);
  CHECK(!result.completed);
  CHECK(result.error.ok());
  CHECK(result.interrupted_by_speech);
  CHECK(result.playback_started);
  // 归属完整：说明是哪条流里的第几次说话，从哪一帧开始。
  CHECK(result.interrupt_notice.stream_id == "mic-barge-in");
  CHECK(result.interrupt_notice.segment_id == 2);
  CHECK(result.interrupt_notice.start_sequence == 17);
  CHECK(result.interrupt_notice.speech_sequence == 20);
  // 打断发生在第 1 帧之后：生成与合成都被显式停止，设备只收到这一帧。
  CHECK(sink.write_count() == 1);
  CHECK(llm.cancel_calls() == 1);
  CHECK(tts.cancel_calls == 1);
  CHECK(llm.delivered() < tokens.size());
  CheckCancelTrace(session);

  // 打断不消耗新语音：会话只取走通知，音频仍留在输入侧等待下一轮。
  CHECK(monitor.pending_speech.size() == 3);
  CHECK(monitor.pending_speech.front().samples.front() == 900);

  // 恢复：新语音作为下一轮的完整输入进入识别，并跑出完整回答。
  SessionTurnInput next;
  next.stream_id = "mic-barge-in";
  next.request_id = "req-barge-resumed";
  next.generation = 2;
  for (const auto& frame : monitor.pending_speech) {
    next.pcm_samples.insert(next.pcm_samples.end(), frame.samples.begin(), frame.samples.end());
  }
  const auto resumed = session.run(next);
  CHECK(resumed.error.ok());
  CHECK(resumed.completed);
  CHECK(!resumed.interrupted_by_speech);
  CHECK(resumed.text == kHitText);
  CHECK(resumed.pcm_frames.size() == 3);
  CHECK(session.state_machine().generation() == 3);
}

// 清理失败不变量：取消受理之后，如果播放组件的停止操作本身失败，会话必须如实报告，不能把
// “取消成功”当成“已经静默”。终态仍然是唯一的取消终态（本轮确实被取消了，清理失败不改写它），
// 但待播数据并没有被丢弃，因此 pending_count() 仍大于 0，cleanup_error 非空。
// 这条用例补上验收条目里的“各自有明确超时或失败结果”：本版播放清理是同步非阻塞的，
// 没有超时分支，于是“清理失败”就是它唯一可能的非成功结果，必须有可观测的落点。
void TestCancelReportsPlaybackCleanupFailure() {
  auto retriever = MakeIndex();
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{1.01, 0.50, 3});
  SessionSlot slot;
  // 12 字节一个 token、片段上限 12 字节：每个 token 一个片段、一帧，取消点落在第 2 帧。
  const std::vector<std::string> tokens = {Repeat('a', 12), Repeat('b', 12), Repeat('c', 12),
                                           Repeat('d', 12)};
  CooperativeLlm llm(tokens, slot, /*cancel_after_token=*/0);
  ScriptedAsr asr(kL2Question, slot, 0);
  RecordingTts tts;
  ManualClock clock;
  // 时钟冻结：取消时刻确实有 2 帧“已写出但尚未播完”的积压，否则待播为 0 会让断言失去区分度。
  CancellingSink sink(clock, /*advance_clock=*/false, slot, /*cancel_at=*/2);
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);
  FailingStopPlayback playback_with_failing_stop(playback);
  SessionRuntimeConfig config;
  config.text_chunk_max_bytes = 12;
  SessionRuntime session(asr, retriever, router, tts, playback_with_failing_stop, &llm, config);
  slot.session = &session;

  const auto result = session.run(TurnInput("mic-cleanup-fail", "req-cleanup-fail", 2));

  CHECK(result.cancelled);
  CHECK(!result.completed);
  // 取消没有被清理失败改写：终态仍是取消，而不是被伪装成一次设备错误。
  CHECK(result.error.ok());
  CHECK(result.terminal_marker == ActivityMarker::kTerminalCancelled);
  CheckCancelTrace(session);
  // 但“清理完成”这一事实不成立：停止失败，待播队列没有被丢弃，已写出的两帧仍留在设备里。
  CHECK(!result.cleanup_error.ok());
  CHECK(result.cleanup_error.code == domain::ErrorCode::kDeviceFailure);
  CHECK(playback_with_failing_stop.stop_calls >= 1);
  CHECK(playback.pending_count() == 2);
  CHECK(result.pcm_frames.size() == 2);
}

}  // namespace

int main() {
  TestCancelWhileListeningStopsFeedingAndSkipsRouting();
  TestCancelAtFinalBoundarySkipsRoutingAndGeneration();
  TestCancelDuringThinkingStopsGenerationAndKeepsOldTokensOut();
  TestCancelDuringSpeakingClearsQueuedPlayback();
  TestLateResultsFromUninterruptibleBackendsAreIsolated();
  TestRepeatedCancelAndLastFrameRaceHaveSingleTerminal();
  TestBargeInDuringGenerationStopsBackendsAndKeepsNewSpeech();
  TestCancelReportsPlaybackCleanupFailure();
  return 0;
}

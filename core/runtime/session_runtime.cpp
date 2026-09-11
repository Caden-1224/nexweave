#include "session_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <utility>

namespace nexweave::runtime {
namespace {

using domain::ErrorCode;
using domain::OperationResult;

// 能力对象拒绝回调注册时的诊断文本；机器决策仍只使用错误码。
constexpr char kAsrCallbackRejected[] = "ASR 未接受回调注册";

// 归一化控制动作：只做 ASCII 小写与去空白，不做语义改写；空串表示无动作。
std::string normalize_action(const std::string& action) {
  std::string output;
  output.reserve(action.size());
  for (const unsigned char character : action) {
    if (std::isspace(character) != 0) {
      continue;
    }
    output.push_back(static_cast<char>(std::tolower(character)));
  }
  return output;
}

// 会话是否已经识别到可回答的文本。全空白文本等价于“没有语音”，必须在路由前
// 收敛：否则静音会被当成一次检索请求，产生没有用户意图的回答。
bool has_speech_text(const std::string& text) {
  return text.find_first_not_of(" \t\r\n") != std::string::npos;
}

}  // namespace

bool is_stop_control_action(const std::string& control_action) {
  const std::string action = normalize_action(control_action);
  return action == "cancel" || action == "stop";
}

ManualPlaybackClock::ManualPlaybackClock(std::int64_t start_ms) noexcept : now_ms_(start_ms) {}

std::int64_t ManualPlaybackClock::now_ms() const noexcept {
  return now_ms_;
}

void ManualPlaybackClock::advance(std::int64_t delta_ms) {
  if (delta_ms <= 0) {
    return;
  }
  now_ms_ += delta_ms;
}

LogicalClockPlayback::LogicalClockPlayback(capability::IAudioSink& renderer,
                                           const IPlaybackClock& clock,
                                           std::int64_t frame_ms) noexcept
    : renderer_(renderer), clock_(clock), frame_ms_(frame_ms) {
  if (frame_ms_ <= 0) {
    frame_ms_ = domain::kAudioFrameDurationMs;
  }
}

OperationResult LogicalClockPlayback::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted, "播放已经开始");
  }
  written_.clear();
  written_count_ = 0;
  played_ = 0;
  error_ = domain::Error{};
  started_ = true;
  stopped_ = false;
  return OperationResult::success();
}

OperationResult LogicalClockPlayback::render(const domain::AudioFrame& frame) {
  // 先校验帧再判断状态：非法帧无论播放是否已经开始都是调用方错误，
  // 错误码不应随内部状态变化而改变。
  const auto validation = domain::validate_audio_frame(frame);
  if (!validation.ok()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "播放帧不符合统一音频契约");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || stopped_) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted, "播放未开始或已停止");
  }
  // 停止已受理：不再写出任何新帧。判定发生在写设备之前，因此“受理停止”到
  // “停止发声”之间至多已有一帧在途，这与统一取消语义声明的并发窗口一致。
  if (cancelled_ != nullptr && cancelled_->load()) {
    return OperationResult::failure(ErrorCode::kCancelled, "停止已受理，不再写出新帧");
  }
  if (!error_.ok()) {
    return OperationResult::failure(error_.code, error_.message);
  }

  // 立即写设备：播放是否“完成”由逻辑时钟判定，而不是由“设备是否肯收”决定。
  // 这样调用方不需要为了写入而先推进时钟，也就不会出现互相等待的死锁。
  const auto written = renderer_.write(frame);
  if (!written.ok()) {
    error_ = written.error;
    return written;
  }
  written_.push_back(frame);
  ++written_count_;
  // 立即按当前设备时间结算一次：真实设备可能在两次交付之间已经播完若干帧，
  // 因此不需要调用方先推进时钟再轮询，就不会出现互相等待的死锁。
  refresh_played_locked();
  return OperationResult::success();
}

void LogicalClockPlayback::refresh_played_locked() {
  // 第 k 帧（0 起）覆盖到累计时长 (k+1)×frame_ms；时钟越过门槛即视为已播完。
  // 判定只读时钟，因此同一帧被重复交付也只会各占一个位置，不会改写已完成的历史。
  while (played_ < written_count_) {
    const std::int64_t due_ms = static_cast<std::int64_t>(played_ + 1) * frame_ms_;
    if (clock_.now_ms() < due_ms) {
      break;
    }
    ++played_;
  }
}

OperationResult LogicalClockPlayback::stop() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  // 丢弃尚未播完的部分：已经写入设备的帧不可撤回，但“还欠多少播放时间”必须清零，
  // 否则取消后的轮次会继续把旧回答算作待播。played_count() 记录的是真实播完的帧数，
  // 因此取消不会把它改成“全部播完”；是否仍有未播完内容由 stopped_ 单独表达。
  stopped_ = true;
  // 释放本轮占用：停止之后必须重新 start() 才能开始下一轮。这样同一播放组件可以
  // 被常驻会话跨轮次复用，而不会因为“上一轮还开着”导致下一轮无法开始。
  started_ = false;
  return OperationResult::success();
}

void LogicalClockPlayback::poll() {
  std::lock_guard<std::mutex> lock(mutex_);
  refresh_played_locked();
}

OperationResult LogicalClockPlayback::error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (error_.ok()) {
    return OperationResult::success();
  }
  return OperationResult::failure(error_.code, error_.message);
}


std::size_t LogicalClockPlayback::played_count() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return played_;
}

std::size_t LogicalClockPlayback::pending_count() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  // 停止后没有“待播”可言：本轮剩余音频已经被丢弃，而不是等待播放。
  if (stopped_) {
    return 0;
  }
  // 尚未播完的帧数 = 已写入设备的帧数 - 已按逻辑时钟播完的帧数。played_ 单调不减且
  // 不会超过 written_count_，因此这里不会出现无符号下溢。
  return written_count_ - played_;
}

std::vector<domain::AudioFrame> LogicalClockPlayback::played_frames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return written_;
}

void LogicalClockPlayback::set_cancelled_flag(const std::atomic<bool>* flag) noexcept {
  cancelled_ = flag;
}

SessionRuntime::SessionRuntime(capability::IAsr& asr, capability::IRag& retriever,
                               backend::FakeRagRouter& router, capability::ITts& tts,
                               IAudioPlayback& playback, SessionRuntimeConfig config)
    : asr_(asr),
      router_(router),
      tts_(tts),
      playback_(playback),
      config_(config) {
  // retriever 只用于明确生命周期关系：路由器持有它的引用，因此检索器必须比路由器
  // 活得久。Session 自己不做检索，命中信息由路由决策携带，故不再保存副本。
  (void)retriever;
}

SessionTurnResult SessionRuntime::run(const SessionTurnInput& input) {
  SessionTurnResult result;

  // 每轮开始时解除上一轮的取消封锁：取消只封锁当时在途的那一代，不能永久锁死
  // 会话。router 的 reset/cancel 都是幂等值操作，因此重复调用安全。
  cancel_requested_.store(false);
  queue_overflow_ = false;
  asr_text_.clear();
  speech_interrupt_ = false;
  speech_interrupt_notice_ = SpeechStartNotice{};
  router_.reset();
  // 清空上一轮遗留的起音通知：那些语音属于本轮或更晚的说话，本轮回答还没有开口，
  // 没有任何旧回答需要被打断；不清空会让新回答刚说出第一帧就被自己打断。
  drain_barge_in_notices();

  if (!state_machine_.dispatch(SessionStateMachine::Event::kAudioStart).ok()) {
    result.error = domain::Error{ErrorCode::kInvalidInput, "会话已有在途轮次，必须先结束或取消"};
    finish_turn(result);
    return result;
  }

  active_stream_id_ = input.stream_id;
  active_request_id_ = input.request_id;
  result.request_id = input.request_id;

  // 常驻采集只在第一轮建立：stream_id 非空且不能中途更换，否则输入序号会串到
  // 另一个流上。后续轮次沿用同一 stream，符合“常驻输入跨回答轮次保持”的约定。
  if (!stream_started_) {
    const auto opened = contract_.start_stream(input.stream_id);
    if (!opened.ok()) {
      result.error = opened.error;
      finish_turn(result);
      return result;
    }
    stream_started_ = true;
  }
  const auto generation = contract_.begin_generation();
  if (!generation.ok()) {
    result.error = generation.error;
    finish_turn(result);
    return result;
  }
  // 代际水位不在本地缓存：状态机在 audio_start 与 cancel 时都会推进水位，本地副本
  // 一旦与它脱节，失败/取消过的轮次就会带着过期代际提交事件并被拒绝。这里统一在
  // 每次提交前读取状态机，使“夹具记录的代际”与“状态机裁决的代际”永远是同一个值。

  // ASR 回调只接受本轮代际的最终文本；取消会推进状态机代际，迟到回调不会把旧
  // 识别结果标成当前轮次。回调在 feed 的调用线程同步执行，因此这里不需要加锁。
  const auto registered = asr_.set_callback([this](const capability::TextEvent& event) {
    if (event.kind == capability::TextEventKind::kFinal) {
      asr_text_ = event.text;
    }
  });
  if (!registered.ok()) {
    // 能力对象拒绝回调注册：本轮没有任何可观察输入，按失败收敛而不是伪造空文本。
    result.error = domain::Error{ErrorCode::kBackendFailure, kAsrCallbackRejected};
    finish_turn(result);
    return result;
  }

  // 输入校验与切帧沿用任务 15 的契约：空样本返回空列表，短尾补零并保留
  // valid_samples，超过 320 样本切成多帧且不静默截断。
  const auto frames = pad_audio_samples(input.pcm_samples);
  if (!frames.ok()) {
    result.error = frames.error;
    finish_turn(result);
    return result;
  }
  if (frames.value->empty()) {
    result.error = domain::Error{ErrorCode::kInvalidInput, "输入音频为空"};
    finish_turn(result);
    return result;
  }

  // 逐帧提交给 ASR：最后一帧带 is_last，Fake ASR 据此发布 final。补零尾部在这里
  // 保持其真实有效样本数（由 pad_audio_samples 计算），ASR 只关心帧本身。
  for (std::size_t index = 0; index < frames.value->size(); ++index) {
    const bool is_last = index + 1 == frames.value->size();
    const auto fed = asr_.feed(frames.value->at(index).frame, is_last);
    if (!fed.ok()) {
      result.error = fed.error;
      finish_turn(result);
      return result;
    }
  }
  result.input_done = true;

  // 空文本或全空白文本表示没有可回答的语音内容：在路由之前收敛，避免把静音
  // 变成一次检索、一次合成或一次空洞回答。
  if (!has_speech_text(asr_text_)) {
    result.error = domain::Error{ErrorCode::kInvalidInput, "ASR 未识别到文本"};
    // 状态机仍处于 Listening：按“无有效输入”收敛，不做无意义的 AsrFinal 迁移。
    finish_turn(result);
    return result;
  }

  const auto heard =
      state_machine_.dispatch(SessionStateMachine::Event::kAsrFinal,
                              state_machine_.generation());
  if (!heard.ok()) {
    result.error = heard.error;
    finish_turn(result);
    return result;
  }

  const auto decision = router_.route(asr_text_);
  if (!decision.ok()) {
    result.error = decision.error;
    finish_turn(result);
    return result;
  }
  result.decision = *decision.value;
  result.route = decision.value->level;

  if (decision.value->level == backend::RagRouteLevel::kL0) {
    // L0 是控制意图而不是知识问题：路由在控制分支已经返回，因此这里既不检索、
    // 也不调用 LLM 和合成。停止类动作按统一取消语义收尾；其余控制动作只被记录
    // 并明确拒绝，绝不退化成一次普通回答。
    if (is_stop_control_action(decision.value->control_action)) {
      // 停止类控制动作是唯一有明确定义的 L0 行为，按统一取消语义收尾；规格不允许
      // 把它降级成普通知识问答，因此这里没有“仅记录并拒绝”的替代策略。
      result.cancelled = true;
      cancel_requested_.store(true);
      state_machine_.dispatch(SessionStateMachine::Event::kCancel,
                              state_machine_.generation());
    } else {
      // 其余控制动作不在本版 L0/L1 语义内：明确拒绝，也不伪造回答。
      result.error = domain::Error{ErrorCode::kInvalidInput, "控制意图不由 L0/L1 语音路径回答"};
    }
    finish_turn(result);
    return result;
  }

  if (decision.value->level != backend::RagRouteLevel::kL1) {
    // kL2/kL3 需要 LLM，属于 Thinking 阶段，由后续任务实现；这里显式拒绝而不
    // 编造回答，避免把未实现的路径伪装成成功。
    result.error = domain::Error{ErrorCode::kInvalidInput, "该路由级别不在 L0/L1 语音路径范围内"};
    finish_turn(result);
    return result;
  }

  result.text = decision.value->direct_answer;
  if (result.text.empty()) {
    result.error = domain::Error{ErrorCode::kBackendFailure, "L1 直答缺少可合成文本"};
    finish_turn(result);
    return result;
  }

  const auto routing_done =
      state_machine_.dispatch(SessionStateMachine::Event::kRouteL0L1, state_machine_.generation());
  if (!routing_done.ok()) {
    result.error = routing_done.error;
    finish_turn(result);
    return result;
  }

  // 回答文本已经定稿：L0/L1 没有 LLM，因此“文本定稿”就是本轮的回答生成结束，
  // 在此提交 kGenerationDone。任务 15 的三个局部完成标记因此对直答同样成立，
  // L2/L3 接入 LLM 时改由 token 流结束时提交同一标记。
  const auto generation_done = contract_.mark_generation_done(contract_.generation());
  if (!generation_done.ok()) {
    result.error = generation_done.error;
    finish_turn(result);
    return result;
  }

  // 播放组件在本轮开始时拿到停止标志的借用引用：判定为真后不得再写出新帧。
  playback_.set_cancelled_flag(&cancel_requested_);
  const auto playback_opened = playback_.start();
  if (!playback_opened.ok()) {
    result.error = playback_opened.error;
    finish_turn(result);
    return result;
  }

  bool playback_started = false;
  std::vector<domain::AudioFrame> delivered;
  tts_.set_callback([this, &result, &delivered, &playback_started](
                        const domain::AudioFrame& frame) {
    // 停止已受理：不再接收新帧，也不把它交给播放组件，避免排队数据在取消后发声。
    if (cancel_requested_.load()) {
      return;
    }
    // 播放队列上限是显式策略：达到上限时本轮明确失败，而不是静默丢帧。
    if (config_.playback_queue_capacity > 0 &&
        playback_.pending_count() >= config_.playback_queue_capacity) {
      queue_overflow_ = true;
      return;
    }
    const auto rendered = playback_.render(frame);
    if (!rendered.ok()) {
      return;
    }
    delivered.push_back(frame);
    if (!playback_started) {
      // 第一帧进入播放组件即证明“播放已开始”，它严格早于合成结束标记，
      // 这就是流式重叠的线性化证据，而不是仅仅同时存在 token 和 PCM。
      playback_started = true;
      contract_.start_playback(contract_.generation());
    }
    // 播放交付边界是打断的线性化点：本帧已经交给播放组件之后才询问是否出现新语音，
    // 因此“打断”与“已播出的声音”不会互相矛盾——已播出的帧永远保留。
    poll_barge_in();
  });

  const auto synthesized = tts_.synthesize(result.text);
  playback_.set_cancelled_flag(nullptr);

  // 合成结束只是局部完成：帧虽然已经写进设备，但设备时间可能在合成期间只推进了
  // 一部分。这里轮询一次播放进度，让“已播出多少”追上当前设备时间，再据此判断本轮
  // 是否真的播完。poll 不写设备、不睡眠，因此不会把“还没播完”伪装成成功。
  if (!cancel_requested_.load() && !queue_overflow_) {
    playback_.poll();
  }

  // pcm_frames 记录的是“已经交付给播放组件、由其负责播放或丢弃”的帧；不能用
  // played_frames() 覆盖它，否则缓冲中的音频会既播不出来也拿不回去。
  result.pcm_frames = delivered;
  result.playback_started = playback_started;
  // 判据是“所有已写入设备的帧都播完了”：合成刚开始时没有写入任何帧，因此还要求
  // 至少播完一帧，避免把“什么都没播”当成完成。这里不调用 stop()，所以计数仍
  // 反映本轮的播放进度。
  const bool all_played = playback_.played_count() > 0 &&
                          playback_.pending_count() == 0;
  result.playback_done = playback_started && !cancel_requested_.load() && !queue_overflow_ &&
                         playback_.error().ok() && all_played;

  if (queue_overflow_) {
    result.pcm_frames.clear();
    result.playback_done = false;
    result.error = domain::Error{ErrorCode::kBackendFailure, "播放队列已满，本轮拒绝继续合成"};
    playback_.stop();
    finish_turn(result);
    return result;
  }
  if (!synthesized.ok()) {
    result.playback_done = false;
    result.error = synthesized.error;
    playback_.stop();
    finish_turn(result);
    return result;
  }
  if (cancel_requested_.load()) {
    result.cancelled = true;
    result.playback_done = false;
    // 取消来源有三类：设备回调里的停止指令、控制意图、以及新语音打断。只有最后
    // 一类会让 interrupted_by_speech 为真，使“回答被打断”和“用户喊停”在结果与
    // 运行证据里可区分，而不是都表现为一次无来源的取消。
    result.interrupted_by_speech = speech_interrupt_;
    if (speech_interrupt_) {
      result.interrupt_notice = speech_interrupt_notice_;
    }
    playback_.stop();
    state_machine_.dispatch(SessionStateMachine::Event::kCancel, state_machine_.generation());
    finish_turn(result);
    return result;
  }
  const auto playback_error = playback_.error();
  if (!playback_error.ok()) {
    result.playback_done = false;
    result.error = playback_error.error;
    playback_.stop();
    finish_turn(result);
    return result;
  }
  if (!playback_started) {
    // 合成成功却没有交付任何帧：没有可播放的输出，不能报告成功。
    result.error = domain::Error{ErrorCode::kBackendFailure, "合成未产生任何 PCM 帧"};
    playback_.stop();
    finish_turn(result);
    return result;
  }
  if (!all_played) {
    // 仍有未播完的帧：设备时间还没覆盖这些音频。本轮不算完成，由调用方推进设备
    // 时间后重跑，或按取消收敛；这里明确失败而不是把“还没播完”写成成功。
    result.error = domain::Error{ErrorCode::kTimeout, "播放尚未完成，仍有未播完的帧"};
    playback_.stop();
    finish_turn(result);
    return result;
  }

  contract_.mark_synthesis_done(contract_.generation());
  const auto speech_done =
      state_machine_.dispatch(SessionStateMachine::Event::kTtsDone, state_machine_.generation());
  if (!speech_done.ok()) {
    result.error = speech_done.error;
    result.playback_done = false;
    finish_turn(result);
    return result;
  }

  result.completed = true;
  finish_turn(result);
  return result;
}

domain::OperationResult SessionRuntime::finish_stream() {
  if (!stream_started_) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "输入流已经关闭");
  }
  // 结束事件使用零有效样本与序号 0：任务 15 的夹具把常驻流的序号只分配给送到夹具的
  // 输入事件，而本路径的逐帧音频由 ASR 消费、不经过夹具，因此关闭流时流内还没有
  // 任何已登记事件，序号必然是 0。这里不额外伪造补零尾帧事件，结束事件只表达
  // “该 stream 不再有新音频”。
  InputStreamEvent event;
  event.stream_id = active_stream_id_;
  event.generation = contract_.generation();
  event.sequence = 0;
  event.kind = InputEventKind::kEnd;
  event.valid_samples = 0;
  const auto closed = contract_.end_stream(event);
  if (!closed.ok()) {
    return closed;
  }
  stream_started_ = false;
  return OperationResult::success();
}

domain::OperationResult SessionRuntime::cancel() noexcept {
  cancel_requested_.store(true);
  if (state_machine_.state() == SessionStateMachine::State::kIdle) {
    return OperationResult::success();
  }
  return state_machine_.dispatch(SessionStateMachine::Event::kCancel);
}

void SessionRuntime::set_barge_in_monitor(IBargeInMonitor* monitor) noexcept {
  barge_in_ = monitor;
}

void SessionRuntime::drain_barge_in_notices() {
  if (barge_in_ == nullptr) {
    return;
  }
  // 监视器每次只交付一条最早通知，因此循环取空。这里取到的通知一律丢弃而不是当成
  // 打断：它们描述的是本轮开始之前就已经出现的说话，没有旧回答需要被打断；
  // 对应的音频仍然留在常驻输入队列里，会作为本轮的输入被取走。
  while (barge_in_->take_speech_started().has_value()) {
  }
}

void SessionRuntime::poll_barge_in() {
  if (barge_in_ == nullptr || cancel_requested_.load()) {
    return;
  }
  const auto notice = barge_in_->take_speech_started();
  if (!notice.has_value()) {
    return;
  }
  // 顺序固定为“记录归属 → 置位停止标志 → 推进状态机”。先记录归属保证结果里一定
  // 有打断原因；先置位再迁移，保证即使迁移因为代际过期而失败，也不会出现
  // “状态机仍显示 Speaking，但停止标志已经置位”这种中间态被后续判定误读。
  speech_interrupt_ = true;
  speech_interrupt_notice_ = *notice;
  cancel_requested_.store(true);
  state_machine_.dispatch(SessionStateMachine::Event::kCancel, state_machine_.generation());
}

const SessionStateMachine& SessionRuntime::state_machine() const noexcept {
  return state_machine_;
}

std::vector<ActivityMarker> SessionRuntime::trace() const {
  return contract_.trace();
}

void SessionRuntime::finish_turn(SessionTurnResult& result) {
  force_idle();
  // 无论成功、取消还是失败，播放组件都必须在本轮结束时停止并释放轮次资源：
  // 成功路径同样要释放，否则常驻会话的下一轮会因为“上一轮还开着”而无法开始播放。
  // stop() 幂等，重复调用无副作用；已写入设备的声音不受影响。
  playback_.stop();
  playback_.set_cancelled_flag(nullptr);
  result.state = state_machine_.state();

  const bool success = result.error.ok() && result.completed && result.playback_done &&
                       result.playback_started;
  const std::uint64_t generation = contract_.generation();
  if (success) {
    // 顺序固定为“文本定稿 → 播放开始 → 合成结束 → 播放结束”。播放完成是本轮唯一
    // 的成功收尾条件，它在任务 15 的夹具里同时产出唯一 kTerminalSucceeded 标记。
    contract_.mark_playback_done(generation);
    result.terminal_marker = ActivityMarker::kTerminalSucceeded;
    return;
  }
  if (result.cancelled) {
    // 取消只产生一个取消终态，且不撤回已经播放的声音：夹具按“受理 → 封锁旧输出
    // → 执行退出 → 播放清理 → 取消终态”的固定顺序记录。
    contract_.cancel(generation);
    result.terminal_marker = ActivityMarker::kTerminalCancelled;
    return;
  }
  // 失败路径也必须有唯一终态，否则调用方无法区分“尚未完成”和“已经失败终止”。
  // 这里借用取消收敛（封锁旧输出、丢弃排队输出），保证失败不会留下半完成轮次；
  // 夹具对“尚未开启代际”的取消会拒绝，因此该调用失败即为空操作。
  contract_.cancel(generation);
  result.terminal_marker = ActivityMarker::kTerminalCancelled;
}

void SessionRuntime::force_idle() {
  const auto current = state_machine_.state();
  if (current == SessionStateMachine::State::kIdle) {
    return;
  }
  if (current != SessionStateMachine::State::kCancelling) {
    // 先进入 Cancelling 再回到 Idle：失败与取消共用同一条收敛路径，轨迹里一定
    // 出现“封锁旧输出”的阶段，而不是直接从中间阶段跳回 Idle 掩盖在途工作。
    state_machine_.dispatch(SessionStateMachine::Event::kCancel);
  }
  state_machine_.dispatch(SessionStateMachine::Event::kCancelComplete);
}

}  // namespace nexweave::runtime

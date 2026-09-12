#include "session_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <optional>
#include <utility>

namespace nexweave::runtime {
namespace {

using domain::Error;
using domain::ErrorCode;
using domain::OperationResult;

// 能力对象拒绝回调注册时的诊断文本；机器决策仍只使用错误码。
constexpr char kAsrCallbackRejected[] = "ASR 未接受回调注册";

// L2 提示词版式版本：检索片段与用户问题按固定版式拼接后交给 LLM。版式变化必须同时
// 改动本常量与对应测试，使“某次回答依据哪一版上下文版式产生”可追溯，而不是让新旧
// 证据混在一起。本版式只是核心层的确定性拼装，不含模型私有对话模板，真实适配器在
// 外层再做各自的包装。
constexpr char kL2PromptVersion[] = "nexweave-l2-v1";

// 下列诊断文本只用于定位，机器决策一律使用错误码。
constexpr char kLlmNotInjected[] = "会话未注入 LLM，L2/L3 轮次不能生成回答";
constexpr char kLlmCallbackRejected[] = "LLM 未接受回调注册";
constexpr char kTextChunkLimitRejected[] = "文本片段字节上限为 0，生成路径拒绝该配置";
constexpr char kIncompleteUtf8Rejected[] = "生成文本不是完整的 UTF-8 序列";
constexpr char kPlaybackBackpressureRejected[] = "播放缓冲达到上限，本轮拒绝继续合成";
constexpr char kPlaybackIncomplete[] = "播放尚未完成，仍有未播完的帧";
constexpr char kNoSynthesisOutput[] = "合成未产生任何 PCM 帧";
constexpr char kEmptyGenerationRejected[] = "生成未产生任何可播放音频，本轮不算完成";
constexpr char kBlankGenerationRejected[] = "生成未产生可合成的文本";

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

// “某个后端本轮确实在途”的作用域标记：构造置位、析构清零。取消传播据此只触碰真正
// 在执行的调用，而不会给本轮没跑过的后端留下副作用。之所以用 RAII 而不是前后成对赋值：
// 生成器与合成器都允许异常向上传播，手写配对会在异常路径上漏掉清零，从而让一次异常
// 在会话里留下“后端仍在途”的假象，下一次取消就会去打一个已经返回的后端。
// 标记本身是原子量：取消可以由设备或能力组件的回调发起，而按类注释的并发约定，那条
// 回调未必与 run() 同线程，普通 bool 的读写在那种调用下是数据竞争。
// 不可嵌套使用：内层析构会提前把标志清零，使外层仍在执行的调用被误判成“已经返回”。
// 本类只在同一处成对使用，因此无需引用计数。
class ActiveRoundGuard final {
 public:
  explicit ActiveRoundGuard(std::atomic<bool>& flag) noexcept : flag_(flag) {
    flag_.store(true);
  }
  ~ActiveRoundGuard() {
    flag_.store(false);
  }
  ActiveRoundGuard(const ActiveRoundGuard&) = delete;
  ActiveRoundGuard& operator=(const ActiveRoundGuard&) = delete;

 private:
  std::atomic<bool>& flag_;
};

// 生成进度转发器：把“后端报告的生成事实”转给会话拥有者注册的观察者。
// 会话层实现 IGenerationProbe（它需要接收后端的事实来驱动分句与播放），而运行证据需要的
// 是同一批事实的副本，因此这里再包一层：后端只认一个探针，探针内部把事件同时交给会话与
// 观察者。观察者可以为空，此时本对象等价于会话自己，不产生任何额外分支。
class RelayGenerationProbe final : public capability::IGenerationProbe {
 public:
  RelayGenerationProbe(capability::IGenerationProbe& inner,
                       capability::IGenerationObserver* observer) noexcept
      : inner_(inner), observer_(observer) {}

  void on_generation_started() override {
    inner_.on_generation_started();
    if (observer_ != nullptr) {
      observer_->on_generation_started();
    }
  }

  void on_token_delivered(const std::string& token) override {
    inner_.on_token_delivered(token);
    if (observer_ != nullptr) {
      observer_->on_token_delivered(token);
    }
  }

  void on_generation_completed() override {
    inner_.on_generation_completed();
    if (observer_ != nullptr) {
      observer_->on_generation_completed();
    }
  }

  void on_generation_failed(const std::string& message) override {
    inner_.on_generation_failed(message);
    if (observer_ != nullptr) {
      observer_->on_generation_failed(message);
    }
  }

 private:
  capability::IGenerationProbe& inner_;
  capability::IGenerationObserver* observer_ = nullptr;
};

// 会话是否已经识别到可回答的文本。全空白文本等价于“没有语音”，必须在路由前
// 收敛：否则静音会被当成一次检索请求，产生没有用户意图的回答。
bool has_speech_text(const std::string& text) {
  return text.find_first_not_of(" \t\r\n") != std::string::npos;
}

// 按路由级别构造交给 LLM 的 prompt。
//   - kL3：没有可用命中，不注入任何未经支持的知识，只提交用户问题本身；
//   - kL2：把检索片段按后端返回顺序（已由路由器按分数排序）拼进上下文区，再提交用户
//     问题，使“回答依据哪几条命中产生”可以从 prompt 直接复算；
//   - kL0/kL1 不走 LLM，返回空串，由调用方判定错误而不是把空 prompt 送进后端。
// 返回值是按值构造的独立字符串，不引用 decision 或 question 的存储。
std::string build_generation_prompt(backend::RagRouteLevel level,
                                    const backend::RagRouteDecision& decision,
                                    const std::string& question) {
  if (level == backend::RagRouteLevel::kL3) {
    return question;
  }
  if (level != backend::RagRouteLevel::kL2) {
    return {};
  }
  std::string prompt = std::string("[") + kL2PromptVersion + "]\ncontext:\n";
  for (const auto& hit : decision.hits) {
    prompt += "- ";
    prompt += hit.text;
    prompt += '\n';
  }
  prompt += "question: ";
  prompt += question;
  return prompt;
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
                               IAudioPlayback& playback, capability::ILlm* llm,
                               SessionRuntimeConfig config)
    : asr_(asr),
      router_(router),
      tts_(tts),
      playback_(playback),
      llm_(llm),
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
  asr_text_.clear();
  speech_interrupt_ = false;
  speech_interrupt_notice_ = SpeechStartNotice{};
  router_.reset();
  // 生成路径的暂存状态同样每轮清空；漏清会让上一轮的回答文本或错误出现在新一轮，
  // 这正是“输出残留”类缺陷的来源，因此与取消标志放在同一处复位。
  llm_text_.clear();
  llm_chunks_.clear();
  llm_error_.reset();
  playback_backpressure_ = false;
  playback_peak_pending_ = 0;
  // 取消传播的簿记同样每轮复位：上一轮的“已经通知过后端”不能让本轮的取消变成静默，
  // 而上一轮残留的“后端在途”标记会让本轮取消去触碰一个并没有在跑的后端。
  llm_round_active_.store(false);
  tts_round_active_.store(false);
  execution_cancel_notified_.store(false);
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
  // 每个帧边界先检查停止标志：能力组件可以在回调里请求取消（例如识别在某一帧上判定
  // “用户喊停”），受理后继续送帧只会把已经被作废的音频接着喂给识别。
  bool all_frames_submitted = true;
  for (std::size_t index = 0; index < frames.value->size(); ++index) {
    if (cancel_requested_.load()) {
      all_frames_submitted = false;
      break;
    }
    const bool is_last = index + 1 == frames.value->size();
    const auto fed = asr_.feed(frames.value->at(index).frame, is_last);
    if (!fed.ok()) {
      result.error = fed.error;
      finish_turn(result);
      return result;
    }
  }
  result.input_done = all_frames_submitted;
  if (cancel_requested_.load()) {
    // Listening 阶段的统一取消（也覆盖紧随其后的 Routing 边界，见头文件第 2 条）：送帧
    // 已经停止，本轮不可能再产生回答，因此既不提交 AsrFinal 迁移，也不检索、不生成、不合成。
    // asr_.cancel() 刻意放在循环之外：IAsr 不允许在 feed 回调中重入，而这里调用栈上已经
    // 没有任何识别回调。迟到的 partial/final 即使已经写进 asr_text_，也不会被任何后续步骤
    // 读取——本函数直接返回，asr_text_ 由下一轮开头清空，这就是“旧识别结果不污染新请求”的落点。
    (void)asr_.cancel();
    result.cancelled = true;
    state_machine_.dispatch(SessionStateMachine::Event::kCancel, state_machine_.generation());
    finish_turn(result);
    return result;
  }

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
      // 刻意走与设备回调、新语音打断同一个 cancel() 入口：置位、向在途后端传播、状态迁移
      // 三件事只有一份实现，将来若在这个分支与路由之间插入任何在途后端调用，也不会漏掉
      // “通知执行退出”这一步。
      result.cancelled = true;
      (void)cancel();
    } else {
      // 其余控制动作不在本版 L0/L1 语义内：明确拒绝，也不伪造回答。
      result.error = domain::Error{ErrorCode::kInvalidInput, "控制意图不由 L0/L1 语音路径回答"};
    }
    finish_turn(result);
    return result;
  }

  if (decision.value->level != backend::RagRouteLevel::kL1) {
    // kL2/kL3 需要 LLM：转交生成路径。它自己负责收尾与终态，因此这里直接返回，
    // 不再走下面的直答分支，避免两条路径同时向同一代际提交完成标记。
    run_generation_turn(result);
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
    playback_.set_cancelled_flag(nullptr);
    finish_turn(result);
    return result;
  }

  bool playback_started = false;
  // 交付回调只把帧交给播放组件；文本定稿、停止判定与计数都在 synthesize 返回后统一
  // 处理，避免在能力回调里做终态决策（回调期间本轮状态尚未收敛）。
  tts_.set_callback([this, &playback_started](const domain::AudioFrame& frame) {
    (void)deliver_frame(frame, playback_started);
  });

  OperationResult synthesized;
  {
    // 合成期间标记 TTS 在途：取消若在某一帧的回调里到达，就会据此把 cancel() 传播给
    // 合成器，让它从下一帧起停止产出，而不是把整段回答算完再整段丢弃。
    ActiveRoundGuard tts_round(tts_round_active_);
    synthesized = tts_.synthesize(result.text);
  }
  // 回调按引用捕获本轮局部量，必须在本函数返回前注销，否则下一轮合成会调用过期回调。
  detach_tts_callback();
  playback_.set_cancelled_flag(nullptr);

  const auto outcome = finalize_playback(synthesized.ok(), playback_backpressure_);
  if (!settle_playback_stage(result, outcome, synthesized)) {
    return result;
  }

  commit_synthesis_done();
  if (!result.error.ok()) {
    result.playback_done = false;
    abort_playback(result, outcome);
    finish_turn(result);
    return result;
  }

  result.completed = true;
  finish_turn(result);
  return result;
}

bool SessionRuntime::deliver_frame(const domain::AudioFrame& frame, bool& started) {
  // 停止已受理：不再接收新帧，也不把它交给播放组件，避免排队数据在取消后发声。
  if (cancel_requested_.load()) {
    return false;
  }
  // 播放缓冲容量是显式策略：达到上限即本轮拒绝继续合成，而不是静默丢帧。判定发生在
  // 写入之前，因此交付的帧数永远不超过容量，峰值也只是“把上限用满”而不是越过上限。
  // 这里不等待、不睡眠：模型回调因此不需要为了等播放腾出空间而阻塞，等待语义由调用方
  // 通过背压结果决定（失败、重试或改为丢弃无关音频）。
  if (config_.playback_queue_capacity > 0 &&
      playback_.pending_count() >= config_.playback_queue_capacity) {
    playback_backpressure_ = true;
    // 已经确定本轮失败：立即让播放组件进入停止态，避免未播完的旧回答继续发声。
    playback_.stop();
    return false;
  }
  // 打断必须在“本帧已经写出”之后消费：交付边界是打断的线性化点，先写出再询问是否
  // 出现新语音，才能保证“已播出的声音”与“打断决定”不矛盾。顺序若反过来，落在最后
  // 一帧上的起音将永远无人消费——之后不再有交付边界，打断事实会丢失。没有新语音时
  // 它只是一次非阻塞查询。
  const auto rendered = playback_.render(frame);
  if (!rendered.ok()) {
    // 设备拒绝本帧：不计入交付、不推进开始标记；具体错误由调用方在合成结束后通过
    // playback_.error() 读取，避免在回调里做终态决策。
    return false;
  }
  poll_barge_in();
  // 峰值在写入之后取：它记录的是“已经写出设备、此刻尚未播完”的帧数最大值，也就是生成
  // 相对播放最多领先了多少帧。用 played_frames() 的规模减去已播完计数，而不是用交付
  // 计数：设备可能在两次交付之间就已经播完若干帧，交付计数会把“曾经在缓冲里”误算成
  // “此刻仍在缓冲里”，从而把峰值虚高到与实际积压无关的数值。
  const auto written = playback_.played_frames().size();
  const auto played = playback_.played_count();
  playback_peak_pending_ =
      std::max(playback_peak_pending_, written > played ? written - played : std::size_t{0});
  if (!started) {
    // 第一帧进入播放组件即证明“播放已开始”。标记提交在这里而不在调用方，是因为它
    // 必须严格早于“文本定稿”，而文本定稿发生在生成结束之后——这正是重叠的线性化点。
    started = true;
    contract_.start_playback(contract_.generation());
  }
  return true;
}

SessionRuntime::PlaybackOutcome SessionRuntime::finalize_playback(bool synthesized_ok,
                                                                  bool backpressure) {
  // 合成结束只是局部完成：帧虽然已经写进设备，但设备时间可能在合成期间只推进了
  // 一部分。这里轮询一次播放进度，让“已播出多少”追上当前设备时间，再据此判断本轮
  // 是否真的播完。poll 不写设备、不睡眠，因此不会把“还没播完”伪装成成功。
  // 停止已受理或发生背压时跳过：那时本轮已经在清理，没有“还欠多少播放时间”可言。
  if (!cancel_requested_.load() && !backpressure) {
    playback_.poll();
  }
  PlaybackOutcome outcome;
  // “播放已开始”的判据是播放组件确实写出过帧。这里读取的是已写出帧快照的规模，而不是
  // 交付计数：两者只在“写设备失败”时不同，而那种情况由调用方按设备错误收敛。
  outcome.started = !playback_.played_frames().empty();
  // 判据是“所有已写入设备的帧都播完了”：合成刚开始时没有写入任何帧，因此还要求
  // 至少播完一帧，避免把“什么都没播”当成完成。这里不调用 stop()，所以计数仍
  // 反映本轮的播放进度。
  outcome.all_played = outcome.started && playback_.played_count() > 0 &&
                       playback_.pending_count() == 0;
  const bool playback_ok = synthesized_ok && !cancel_requested_.load() && !backpressure &&
                           playback_.error().ok();
  outcome.all_played = outcome.all_played && playback_ok;
  return outcome;
}

bool SessionRuntime::settle_playback_stage(SessionTurnResult& result,
                                            const PlaybackOutcome& outcome,
                                            const domain::OperationResult& synthesized) {
  result.pcm_frames = playback_.played_frames();
  result.playback_started = outcome.started;
  result.playback_done = outcome.all_played;
  result.peak_pending_frames = playback_peak_pending_;
  result.backpressure = playback_backpressure_;

  if (playback_backpressure_) {
    // 播放缓冲达到上限：本轮以背压失败收敛。已经写出设备的帧仍留在 pcm_frames 里
    // （它们确实交付过），未播完的部分被丢弃——这就是“有界等待”的可观察代价。
    result.playback_done = false;
    result.error = domain::Error{ErrorCode::kBackendFailure, kPlaybackBackpressureRejected};
    abort_playback(result, outcome);
    finish_turn(result);
    return false;
  }
  if (cancel_requested_.load()) {
    // 取消排在合成/生成失败之前：取消一旦受理，本轮就已经是取消，被取消的后端随后返回的
    // kCancelled（或它自己的失败）只是取消的结果，不是独立故障；顺序若反过来，一次正常
    // 取消会被报成后端错误，调用方就分不清“用户停止”和“节点坏掉”。
    result.cancelled = true;
    result.playback_done = false;
    // 三类取消（设备回调里的停止指令、控制意图、新语音打断）共用这条收敛路径，只有
    // 新语音打断携带归属，使“回答被打断”和“用户喊停”在证据里可区分。
    attach_interrupt_notice(result);
    abort_playback(result, outcome);
    // 取消必须显式推进状态机：轨迹里要出现 Cancelling，而不是从 Thinking/Speaking
    // 直接跳回 Idle，否则“旧输出已封锁”这一事实会消失。
    state_machine_.dispatch(SessionStateMachine::Event::kCancel, state_machine_.generation());
    finish_turn(result);
    return false;
  }
  if (!synthesized.ok()) {
    result.playback_done = false;
    result.error = synthesized.error;
    abort_playback(result, outcome);
    finish_turn(result);
    return false;
  }
  const auto playback_error = playback_.error();
  if (!playback_error.ok()) {
    // 设备错误优先于“没有交付帧”报告：它解释了为什么没有帧可播，比后者更接近根因。
    result.playback_done = false;
    result.error = playback_error.error;
    abort_playback(result, outcome);
    finish_turn(result);
    return false;
  }
  if (!outcome.started) {
    // 成功收尾却没有交付任何帧：没有可播放的输出，不能报告成功。直答为空与生成只产出
    // 标点都会走到这里，因此不需要另设一条按文本内容判断的分支。
    result.error = domain::Error{ErrorCode::kBackendFailure, kNoSynthesisOutput};
    abort_playback(result, outcome);
    finish_turn(result);
    return false;
  }
  if (!outcome.all_played) {
    // 仍有未播完的帧：设备时间还没覆盖这些音频。本轮不算完成，由调用方推进设备时间后
    // 重跑或按取消收敛；这里明确失败而不是把“还没播完”写成成功。
    result.error = domain::Error{ErrorCode::kTimeout, kPlaybackIncomplete};
    abort_playback(result, outcome);
    finish_turn(result);
    return false;
  }
  return true;
}

void SessionRuntime::commit_synthesis_done() {
  contract_.mark_synthesis_done(contract_.generation());
  const auto speech_done =
      state_machine_.dispatch(SessionStateMachine::Event::kTtsDone, state_machine_.generation());
  if (!speech_done.ok()) {
    llm_error_ = speech_done.error;
  }
}

void SessionRuntime::abort_playback(SessionTurnResult& result,
                                    const PlaybackOutcome& outcome) {
  // 顺序固定为“停止播放 → 填入证据”。先停止保证返回后不会再有新帧写出；证据在停止
  // 之后读取，如实反映“停止前交付了多少、播完了多少”。
  // 停止的返回值必须留下来：清理失败意味着“待播数据已经被丢弃”这一事实并不成立，
  // 此时把取消报告成已经静默就是虚假承诺。只记第一处失败，避免收尾里的第二次 stop()
  // 覆盖根因；终态本身不受影响，因为本轮确实已经被取消。
  const auto stopped = playback_.stop();
  if (!stopped.ok() && result.cleanup_error.ok()) {
    result.cleanup_error = stopped.error;
  }
  result.pcm_frames = playback_.played_frames();
  result.playback_started = outcome.started;
  result.playback_done = false;
}

void SessionRuntime::detach_tts_callback() {
  // 以“什么都不做”的回调覆盖借用：既有实现把空回调定义为非法注册，因此不能靠传空来
  // 注销；覆盖之后接收方不再持有本轮局部量的引用，回调也不会再进入本轮状态。
  tts_.set_callback([](const domain::AudioFrame&) {});
}

void SessionRuntime::attach_interrupt_notice(SessionTurnResult& result) {
  // 设备回调里的停止指令、控制意图与“用户新语音”都以取消终态收敛，只有最后一类
  // 携带归属信息，使“回答被打断”和“用户喊停”在结果与运行证据里可区分。
  result.interrupted_by_speech = speech_interrupt_;
  if (speech_interrupt_) {
    result.interrupt_notice = speech_interrupt_notice_;
  }
}

void SessionRuntime::run_generation_turn(SessionTurnResult& result) {
  if (llm_ == nullptr) {
    // 未注入 LLM 却走到 L2/L3：明确失败，绝不伪造回答。这是“能力缺失”而不是“输入
    // 非法”，因此用后端失败码，调用方可以据此决定补齐依赖还是回退到直答策略。
    result.error = domain::Error{ErrorCode::kBackendFailure, kLlmNotInjected};
    finish_turn(result);
    return;
  }
  if (config_.text_chunk_max_bytes == 0) {
    // 0 在本配置里表示“只按标点切分”，代价是待合成文本与整段回答等长、没有上界。
    // 生成路径不接受这种配置：它会让“文本缓冲有明确容量”这一条不变量失效。受控测试
    // 若需要不限长行为，应改用足够大的具体字节数。
    result.error = domain::Error{ErrorCode::kInvalidInput, kTextChunkLimitRejected};
    finish_turn(result);
    return;
  }

  const std::string question = asr_text_;
  const std::string prompt = build_generation_prompt(result.route, result.decision, question);
  if (prompt.empty()) {
    result.error = domain::Error{ErrorCode::kBackendFailure, "该路由级别没有可用的生成提示词"};
    finish_turn(result);
    return;
  }

  // 状态机迁移先于任何能力调用：Thinking 表示“已经进入生成阶段”，即使生成立即失败，
  // 轨迹也能解释本轮去过哪里。迁移失败说明代际或状态已经过期，不再继续调用后端。
  const auto routed = state_machine_.dispatch(SessionStateMachine::Event::kRouteL2L3,
                                              state_machine_.generation());
  if (!routed.ok()) {
    result.error = routed.error;
    finish_turn(result);
    return;
  }

  // 生成进度观测是可选接缝：后端同时实现 IGenerationProbe 时（确定性夹具即是如此）
  // 由会话在生成开始前挂上、生成返回后立即摘掉。注册责任放在会话而不是调用方，是因为
  // “本轮”只有会话知道：调用方跨轮次挂探针会把上一轮的迟到通知一起收进来；而只在
  // generate 期间挂着，可以保证后端持有的借用指针活不过一次生成。
  auto* const probe = dynamic_cast<capability::IGenerationProbe*>(llm_);
  // 转发器的作用域严格收在一次生成之内：后端持有的借用指针因此永远指向本函数栈上的对象，
  // 生成本身返回后不可能再有迟到通知打到已经析构的转发器上。
  std::optional<RelayGenerationProbe> relay;
  if (probe != nullptr) {
    relay.emplace(*probe, generation_observer_);
    llm_->set_progress_probe(&*relay);
    relay->on_generation_started();
  }

  // 播放组件在本轮拿到停止标志的借用引用：判定为真后不得再写出新帧。
  playback_.set_cancelled_flag(&cancel_requested_);
  const auto playback_opened = playback_.start();
  if (!playback_opened.ok()) {
    result.error = playback_opened.error;
    playback_.set_cancelled_flag(nullptr);
    finish_turn(result);
    return;
  }

  // 每轮一个全新的分句器实例：它按引用捕获本轮局部状态，跨轮复用会把上一轮的未完成
  // 缓冲带进新回答。容量取配置值，因此待合成文本的上界与回答长度无关。
  TextChunker chunker(config_.text_chunk_max_bytes);
  bool playback_started = false;
  // TTS 回调与 L0/L1 共用同一条交付边界，因此背压检查、打断判定与峰值统计只有一份：
  // 合成阶段的每一帧都按相同规则决定“交付、拒绝还是丢弃”。
  tts_.set_callback([this, &playback_started](const domain::AudioFrame& frame) {
    (void)deliver_frame(frame, playback_started);
  });

  // token 回调按“先交付、后统计”的顺序处理：token 一旦到达就立刻进入分句与合成，
  // 因此“首段播放”可以发生在 generate 返回之前；统计只记录事实，不改变交付顺序。
  const auto token_callback = [this, &chunker](const capability::TextEvent& event) {
    if (llm_error_.has_value() || cancel_requested_.load() || playback_backpressure_) {
      // 本轮已经不可能成功：不再分句、不再合成。继续处理只会让回调耗时随输出长度
      // 增长，而不会产生任何可交付的结果。
      return;
    }
    if (event.kind == capability::TextEventKind::kError) {
      llm_error_ = event.error.ok()
                       ? domain::Error{ErrorCode::kBackendFailure, "LLM 报错事件缺少错误码"}
                       : event.error;
      return;
    }
    if (event.kind == capability::TextEventKind::kDone) {
      // 局部完成：这里的 done 只表示“文本不再增长”，不代表播放或会话已经完成，
      // 因此不在这里提交任何完成标记；标记由文本定稿与播放判定分别提交。
      return;
    }
    if (event.kind == capability::TextEventKind::kPartial) {
      // 识别类中间结果不属于生成回答：混入会让同一段文字被合成两次，因此显式忽略。
      return;
    }
    if (event.kind != capability::TextEventKind::kToken) {
      return;
    }
    llm_text_ += event.text;
    if (!has_speech_text(llm_text_)) {
      // 到目前为止只有空白：等价于“没有可合成文本”。空白帧虽然能通过统一音频契约，
      // 但对着空气播一段静音不是一次回答，因此这里不合成、也不交付，直接记录失败并在
      // 收尾时拒绝本轮。判定放在分句之前，避免空白被当作正常文本切出片段送进合成器。
      llm_error_ = domain::Error{ErrorCode::kBackendFailure, kBlankGenerationRejected};
      return;
    }
    // 片段列表每帧复用同一个成员容器：清空后 feed 只追加本次切出的片段，因此“帧”与
    // “片段”不会错位，也不会保留上一帧的片段。
    llm_chunks_.clear();
    chunker.feed(event.text, llm_chunks_);
    for (const auto& chunk : llm_chunks_) {
      if (!synthesize_chunk(chunk)) {
        return;
      }
    }
  };

  const auto registered = llm_->set_callback(token_callback);
  if (!registered.ok()) {
    // 生成回调注册被拒绝：本轮没有任何文本来源，按失败收敛。此时 TTS 已注册回调，
    // 必须注销，否则它会持有一个指向本轮局部量的引用。
    llm_error_ = domain::Error{ErrorCode::kBackendFailure, kLlmCallbackRejected};
    detach_tts_callback();
    playback_.set_cancelled_flag(nullptr);
    result.error = *llm_error_;
    abort_playback(result, finalize_playback(false, playback_backpressure_));
    finish_turn(result);
    return;
  }

  OperationResult generated;
  bool generation_threw = false;
  {
    // 生成期间标记 LLM 在途：取消若在 token 回调（含由该 token 触发的合成与播放回调）
    // 里到达，就会据此把 cancel() 传播给生成器，使它从下一个 token 起停止产出。
    // 作用域刻意收在这一次 generate 调用内：之后的尾段刷新属于合成阶段，那时的取消
    // 应当传导给合成器，而不是继续打在一个已经返回的生成器上。
    ActiveRoundGuard llm_round(llm_round_active_);
    try {
      generated = llm_->generate(prompt);
    } catch (const std::exception& error) {
      // 后端抛出异常：按后端失败收敛，但播放、回调与状态机仍必须走统一的清理路径，
      // 因此先记录错误，让控制流继续到收尾，而不是让异常穿过本对象。
      llm_error_ = domain::Error{ErrorCode::kBackendFailure,
                                 std::string("LLM 生成抛出异常: ") + error.what()};
      generation_threw = true;
    } catch (...) {
      llm_error_ = domain::Error{ErrorCode::kBackendFailure, "LLM 生成抛出未知异常"};
      generation_threw = true;
    }
  }
  if (probe != nullptr) {
    // 只报告真实发生的事实：正常返回才算“生成完成”，异常路径报告失败。把异常也报成
    // 完成会让“首段播放早于生成结束”这条证据在失败轮次里也成立，从而失去区分能力。
    if (generation_threw) {
      probe->on_generation_failed("generate threw");
    } else if (generated.ok() && !llm_error_.has_value()) {
      probe->on_generation_completed();
    }
    // 立即摘掉借用：后端不再持有指向本次生成观测者的指针，轮次之间也就没有悬空引用。
    llm_->set_progress_probe(nullptr);
  }

  if (!generation_threw) {
    // 收尾刷新只在这里调用一次，且只在生成正常返回时调用：取消或失败路径下尾段是否
    // 完整无法确认，把它合成出来等于把截断的文本当成完整回答。
    std::vector<TextChunk> tail;
    chunker.flush(tail);
    for (const auto& chunk : tail) {
      if (!synthesize_chunk(chunk)) {
        break;
      }
    }
    if (chunker.incomplete_utf8() && !llm_error_.has_value()) {
      // 收尾时缓冲仍停在半个字符上：生成文本不是合法 UTF-8。这里拒绝而不是丢弃半截
      // 字节，否则证据里会出现“回答少了一个字”却没有任何错误的情况。
      llm_error_ = domain::Error{ErrorCode::kInvalidInput, kIncompleteUtf8Rejected};
    }
    if (!has_speech_text(llm_text_) && !llm_error_.has_value()) {
      // 全空白回答等价于“没有可合成文本”（正常路径已在 token 回调里拦下，这里兜住
      // “一个字都没到达”的情况）：与 ASR 路径同序，在合成之前收敛。
      llm_error_ = domain::Error{ErrorCode::kBackendFailure, kBlankGenerationRejected};
    }
    if (!generated.ok() && !llm_error_.has_value()) {
      // 生成返回失败且回调没有记下更早的错误：以返回码为准，避免把失败报告成完成。
      llm_error_ = generated.error;
    }
  }

  // 注销 TTS 回调与停止标志借用必须在任何进一步判定之前完成：此后本函数不再产生新
  // 交付，播放组件也回到“本轮可以停止”的状态。
  detach_tts_callback();
  playback_.set_cancelled_flag(nullptr);

  result.text = llm_text_;
  // 生成侧的错误在裁决之前挂到 operation result 上，使两条路径共用同一段判定顺序：
  // 背压 → 取消 → 生成/合成失败 → 设备错误 → 无输出 → 未播完。取消排在生成/合成失败
  // 之前，是因为被取消的后端随后返回的 kCancelled 是取消的结果而不是独立故障，理由见
  // settle_playback_stage 的注释。
  OperationResult generated_result;
  if (llm_error_.has_value()) {
    generated_result = OperationResult::failure(llm_error_->code, llm_error_->message);
  } else if (!generated.ok()) {
    generated_result = generated;
  }
  const auto outcome = finalize_playback(generated_result.ok(), playback_backpressure_);
  if (!settle_playback_stage(result, outcome, generated_result)) {
    return;
  }

  // 文本定稿：生成与分句都已完成，且所有已交付音频都播完了，才提交 kGenerationDone
  // 并进入 Speaking。顺序固定为“播放开始 → 文本定稿 → 合成结束 → 播放结束”，其中
  // “播放开始早于文本定稿”正是生成与播放重叠的可复现证据。
  const auto generation_done = contract_.mark_generation_done(contract_.generation());
  if (!generation_done.ok()) {
    result.error = generation_done.error;
    result.playback_done = false;
    abort_playback(result, outcome);
    finish_turn(result);
    return;
  }
  const auto thinking_done =
      state_machine_.dispatch(SessionStateMachine::Event::kLlmDone, state_machine_.generation());
  if (!thinking_done.ok()) {
    result.error = thinking_done.error;
    result.playback_done = false;
    abort_playback(result, outcome);
    finish_turn(result);
    return;
  }
  commit_synthesis_done();
  if (result.error.ok()) {
    result.completed = true;
  }
  if (!result.completed) {
    result.playback_done = false;
    abort_playback(result, outcome);
    finish_turn(result);
    return;
  }
  finish_turn(result);
}

bool SessionRuntime::synthesize_chunk(const TextChunk& chunk) {
  // 停止已受理：不再启动新的合成。kCancelled 与“合成失败”是两件事，因此下面的失败
  // 分支要按停止标志区分，不能把取消造成的提前返回记成合成器故障。
  if (cancel_requested_.load()) {
    return false;
  }
  domain::OperationResult synthesized;
  {
    // 合成期间标记 TTS 在途：取消若在某一帧的回调里到达，就会据此把 cancel() 传播给
    // 合成器，让它从下一帧起停止产出。片段是逐句合成的，被取消时后续片段不再开始。
    ActiveRoundGuard tts_round(tts_round_active_);
    synthesized = tts_.synthesize(chunk.text);
  }
  if (!synthesized.ok()) {
    if (cancel_requested_.load()) {
      // 合成是被本轮取消打断的：这是取消的结果，不是独立故障。把它写成 llm_error_
      // 会让一次正常取消在终态裁决里被报成后端错误，调用方也就分不清“用户停止”和“节点坏掉”。
      return false;
    }
    // 合成失败：记录第一处失败并停止后续片段。继续合成只会把更多音频交付给一个已经
    // 无法成功收尾的轮次，既浪费算力也让“哪一处失败”变得难以追溯。
    if (!llm_error_.has_value()) {
      llm_error_ = synthesized.error;
    }
    return false;
  }
  return true;
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
  // 先传播停止再迁移状态：传播的线性化点由此落在“状态机已经显示 Cancelling”之前完成，
  // 任何在后端回调里读到的阶段都不会出现“仍在 Thinking/Speaking 但后端已经停了”的反向错觉。
  // 注意这两步的幂等性并不相同：传播由 execution_cancel_notified_ 去重，重复调用是空操作；
  // 状态迁移则不是幂等的——已经处于 Cancelling 时再次 dispatch(kCancel) 会返回 kInvalidInput，
  // 如实表达“本次调用没有改变任何状态”，并且不会产生第二个终态。
  notify_execution_cancel();
  if (state_machine_.state() == SessionStateMachine::State::kIdle) {
    return OperationResult::success();
  }
  return state_machine_.dispatch(SessionStateMachine::Event::kCancel);
}

void SessionRuntime::notify_execution_cancel() noexcept {
  // exchange 而不是“先读后写”：取消可能由设备或能力组件的回调发起，那条回调未必与 run()
  // 同线程；check-then-set 会让两次并发取消都通过判断，从而把“一次取消”放大成两次后端
  // 副作用，与“每轮至多传播一次”的承诺不符。
  if (execution_cancel_notified_.exchange(true)) {
    return;
  }
  // 顺序固定为“先内层合成、后外层生成”。生成器的 token 回调会同步驱动合成与播放，
  // 因此取消到达时两者可能同时在途；先让正在产出音频的合成器停下，可以少合成一帧，
  // 再让生成器停止产出 token。反过来做也能收敛，但会多交付一段注定被丢弃的音频。
  // 两次调用都依赖 capability/backend.hpp 声明的重入例外：ILlm 与 ITts 的 cancel() 必须
  // 可在投递回调内被调用且非阻塞。这是能力契约的一部分，适配器不满足即为不合规，会话不
  // 为不合规的适配器准备降级路径。
  if (tts_round_active_.load()) {
    (void)tts_.cancel();
  }
  if (llm_round_active_.load() && llm_ != nullptr) {
    (void)llm_->cancel();
  }
}

void SessionRuntime::set_barge_in_monitor(IBargeInMonitor* monitor) noexcept {
  barge_in_ = monitor;
}

void SessionRuntime::set_generation_observer(capability::IGenerationObserver* observer) noexcept {
  generation_observer_ = observer;
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
  // 顺序固定为“记录归属 → 置位停止标志 → 传播后端停止 → 推进状态机”。先记录归属保证
  // 结果里一定有打断原因；先置位再迁移，保证即使迁移因为代际过期而失败，也不会出现
  // “状态机仍显示 Speaking，但停止标志已经置位”这种中间态被后续判定误读。
  // 传播放在迁移之前：后端停止是“执行退出”的事实，状态机的 Cancelling 是对它的登记，
  // 先有事实再登记，证据链才不会被解释成“先宣布停止、之后才真的停”。
  speech_interrupt_ = true;
  speech_interrupt_notice_ = *notice;
  cancel_requested_.store(true);
  notify_execution_cancel();
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
  // 与 abort_playback 共用同一处失败记录：取消/失败路径上 abort_playback 已经调用过一次
  // stop()，这里只补记它的失败，不覆盖更早的根因。
  const auto stopped = playback_.stop();
  if (!stopped.ok() && result.cleanup_error.ok()) {
    result.cleanup_error = stopped.error;
  }
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

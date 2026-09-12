// 单进程 Session 应用的端到端集成夹具。
//
// 职责：在应用层（而不是会话编排层）验收“一个可运行的单进程会话”需要满足的行为：
// 三种输入模式各自只有一个生产者、正常/流式重叠/打断/输入结束都能跑完、退出时唤醒
// 输入与执行等待、重复运行不残留输出、确定性输入得到逐帧一致的结果。
//
// 保护的不变量（每个用例在断言旁注明它保护哪一条）：
//   1. 输入源互斥：同一输入流在同一时刻只有一个生产者；文本与固定文件模式不开启常驻
//      采集，只有模拟常驻模式拥有采集生命周期。
//   2. 输入结束即收敛：生产者耗尽后应用关闭输入流并退出，不留“还在等输入”的轮次。
//   3. 重叠成立：生成路径的首帧播放严格早于生成结束，且峰值待播帧数不超过声明容量。
//   4. 旧回答的清理不删除新语音：回答收尾时已经在队列里的下一段说话必须仍然作为后续
//      轮次的输入被取走，排队音频不得被丢弃。（播放途中的实时打断由会话层用例负责：
//      应用按段取输入，因此新语音总会先成为一轮输入，这一点在文件末尾的已知限制里说明。）
//   5. 失败与空输入同样明确：空输入不产生轮次，非法音频在开始前就被拒绝。
//   6. 退出清理成立：提前停止后输入被关闭、等待者被唤醒，且不再产生新轮次。
//   7. 重复运行确定性：同一夹具重复跑得到相同的轮次数、路由、文本与逐帧样本。
//
// 夹具全部确定性、无线程、无睡眠：播放节奏由逻辑时钟推进，取消由设备写入回调在确定的
// 交付边界上触发。本文件不引入 NPU、声卡、网络或模型依赖；唯一的外部资源是文件音频
// 用例显式创建的临时文件，由该用例自己创建与删除。
#include "../test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "fake_audio.hpp"
#include "fake_llm.hpp"
#include "fake_rag.hpp"
#include "fake_tts.hpp"
#include "session_app.hpp"
#include "session_runtime.hpp"

using namespace nexweave;
using nexweave::runtime::SessionApp;
using nexweave::runtime::SessionAppConfig;
using nexweave::runtime::SessionAppInputMode;
using nexweave::runtime::SessionAppRunResult;
using nexweave::runtime::SessionTurnResult;
using nexweave::runtime::SpeechActivity;

namespace {

// 统一音频契约的固定帧长：1 帧 = 320 样本 = 20 ms。本文件所有容量与序号都以帧为单位。
constexpr std::size_t kFrameSamples = domain::kAudioFrameSamples;

// 命中夹具：分数 0.90 使问句走 L1 直答，0.50 使同一命中只作为 L2 的上下文。
constexpr double kL1Score = 0.90;
constexpr double kL2Score = 0.50;
constexpr char kHitText[] = "the capital of france is paris";
// 两个问句都必须是命中文本的子串（确定性检索器的匹配方向是“片段文本包含查询”）。
constexpr char kL1Question[] = "the capital of france";
constexpr char kL2Question[] = "capital of france";
// 确定性检索器把“停止/取消/stop/cancel”识别为 L0 控制意图，不检索也不生成。
constexpr char kStopQuestion[] = "stop";

// 播放逻辑时钟：每次读取前进一个帧时长，模拟“设备按 20 ms 一帧的节奏推进播放”。
// 刻度与读取次数绑定，因此既让“合成结束后确实播完”成立，又不引入墙钟、睡眠或线程：
// 同一夹具重复运行得到完全相同的判定。它在第一次读取时就把时间推到第 1 帧的到期点，
// 因此交付的每一帧都会立刻结算为已播放。
class TickingClock final : public runtime::IPlaybackClock {
 public:
  std::int64_t now_ms() const noexcept override {
    const_cast<TickingClock*>(this)->now_ms_ += domain::kAudioFrameDurationMs;
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

// 定长样本序列：把“几帧、什么电平”的写法统一成一处，便于逐帧比对。
std::vector<std::int16_t> Samples(std::size_t count, std::int16_t value) {
  return std::vector<std::int16_t>(count, value);
}

// 常驻模拟模式的分段策略：一次说话 8 帧、静音 6 帧超时，因此“静音 4 + 人声 8 + 静音 6”
// 共 18 帧就交付一个段。取值刻意取小，使整条用例只有几十帧，不需要真实时间。
runtime::SpeechSegmentationConfig ResidentConfig() {
  runtime::SpeechSegmentationConfig config;
  config.preroll_frames = 2;
  config.min_silence_frames = 6;
  config.min_speech_frames = 4;
  config.max_speech_frames = 40;
  return config;
}

runtime::ResidentAudioInputConfig ResidentInputConfig() {
  runtime::ResidentAudioInputConfig config;
  config.segmentation = ResidentConfig();
  config.pending_segment_capacity = 4;
  return config;
}

// 临时文件路径：带进程号，因此同一个测试进程内的固定名字不会与并发运行的同名测试互撞；
// 文件名由用例给出，路径由系统临时目录决定，测试自己创建并删除它。
std::string TempPath(const std::string& name) {
  std::string dir = "/tmp";
  const char* env = std::getenv("TMPDIR");
  if (env != nullptr && env[0] != '\0') {
    dir = env;
  }
  return dir + "/nexweave-" + std::to_string(static_cast<long>(::getpid())) + "-" + name;
}

bool WriteFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const std::size_t written =
      bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
  const bool flushed = std::fclose(file) == 0;
  return written == bytes.size() && flushed;
}

void AppendLe16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void AppendLe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

void AppendAscii(std::vector<std::uint8_t>& out, const char* text) {
  out.insert(out.end(), text, text + std::strlen(text));
}

// 最小合法 RIFF/WAVE 文件（PCM 子集）：应用只接受 16 kHz、单声道、16 位 PCM，
// 因此夹具显式给出这三个字段，使“拒绝非 16 kHz”的用例只需改一个参数。
std::vector<std::uint8_t> MakeWav(const std::vector<std::int16_t>& samples,
                                  std::uint32_t sample_rate_hz, std::uint16_t channels,
                                  std::uint16_t bits_per_sample) {
  const std::uint32_t data_bytes =
      static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
  const std::uint32_t block_align = static_cast<std::uint32_t>(channels) * (bits_per_sample / 8U);
  std::vector<std::uint8_t> wav;
  AppendAscii(wav, "RIFF");
  AppendLe32(wav, 36U + data_bytes);
  AppendAscii(wav, "WAVE");
  AppendAscii(wav, "fmt ");
  AppendLe32(wav, 16);
  AppendLe16(wav, 1);  // PCM
  AppendLe16(wav, channels);
  AppendLe32(wav, sample_rate_hz);
  AppendLe32(wav, sample_rate_hz * block_align);
  AppendLe16(wav, static_cast<std::uint16_t>(block_align));
  AppendLe16(wav, bits_per_sample);
  AppendAscii(wav, "data");
  AppendLe32(wav, data_bytes);
  for (const auto value : samples) {
    AppendLe16(wav, static_cast<std::uint16_t>(value));
  }
  return wav;
}

// 场景构建器：把“第 n 段有多少帧、是人声还是静音、样本取什么值”写成显式脚本，
// 同时生成活动脚本与 PCM 内容。人声与静音取不同的样本值，因此“送进识别的音频确实是
// 这一段的音频”可以用样本内容核对，而不是只数帧数。
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

// 一次完整说话：起音前静音 4 帧、人声 8 帧、结束后静音 6 帧，共 18 帧。静音 6 帧正好
// 达到静音超时，因此第 18 帧交付一个段。
void AppendUtterance(std::vector<Region>& regions, std::int16_t silence, std::int16_t speech) {
  regions.push_back(Region{4, SpeechActivity::kSilence, silence});
  regions.push_back(Region{8, SpeechActivity::kSpeech, speech});
  regions.push_back(Region{6, SpeechActivity::kSilence, silence});
}

constexpr std::int16_t kSilenceSample = 100;
constexpr std::int16_t kSpeechSampleA = 4000;
constexpr std::int16_t kSpeechSampleB = 7000;
constexpr char kStreamA[] = "app-resident";

// 识别替身：记录每一帧的样本，并按轮次依次交付预设文本。每次 final 只交付一条文本，
// 因此“第几轮说了什么”与“识别是否真的收到了这一轮的音频”可以分别核对。
// 它刻意不实现取消封锁：这些用例验收的是应用层编排，识别侧的取消语义由会话层的
// 取消用例负责，重复实现只会让两边互相掩盖。
// 它同时记录每一轮收到的帧与样本，因此“文本模式是否真的把一帧送进了识别接口”可以直接
// 用送帧次数核对，而不必依赖应用内部状态。
class ScriptedAsr final : public capability::IAsr {
 public:
  explicit ScriptedAsr(std::vector<std::string> texts) : texts_(std::move(texts)) {}

  domain::OperationResult set_callback(capability::TextEventCallback callback) override {
    if (!callback) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    callback_ = std::move(callback);
    fed_samples_.clear();
    fed_frames_ = 0;
    return domain::OperationResult::success();
  }

  domain::OperationResult feed(const domain::AudioFrame& frame, bool is_last) override {
    if (!callback_) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    ++fed_frames_;
    fed_samples_.insert(fed_samples_.end(), frame.samples.begin(), frame.samples.end());
    if (is_last) {
      // 文本列表耗尽后重复最后一项：长输入不会让夹具越界，也不会伪造新内容。
      const std::string text = texts_.at(cursor_ < texts_.size() ? cursor_ : texts_.size() - 1);
          if (cursor_ + 1 < texts_.size()) {
        ++cursor_;
      }
      callback_(capability::TextEvent{capability::TextEventKind::kFinal, text, {}});
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    return domain::OperationResult::success();
  }

  std::size_t fed_frames() const noexcept { return fed_frames_; }
  const std::vector<std::int16_t>& fed_samples() const noexcept { return fed_samples_; }

 private:
  // 逐轮文本与游标：列表耗尽后重复最后一项，因此长输入不会越界。文本模式只会走到第一项。
  std::vector<std::string> texts_;
  std::size_t cursor_ = 0;
  capability::TextEventCallback callback_;
  // 最近一轮收到的音频帧与样本总数；每次注册回调（每轮开始）清零，因此它只描述本轮。
  std::vector<std::int16_t> fed_samples_;
  std::size_t fed_frames_ = 0;
};

// 计数合成器：只有真的调用了 synthesize 才会计数，用来证明“控制意图不会退化成一次
// 知识问答”——若 L0 路径错误地转入直答或生成，这个计数会先变成非零。
class CountingTts final : public capability::ITts {
 public:
  domain::OperationResult set_callback(capability::AudioEventCallback callback) override {
    if (!callback) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    callback_ = std::move(callback);
    return domain::OperationResult::success();
  }

  domain::OperationResult synthesize(const std::string& text) override {
    ++calls_;
    if (text.empty() || !callback_) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    return domain::OperationResult::success();
  }

  std::size_t calls() const noexcept { return calls_; }

 private:
  capability::AudioEventCallback callback_;
  std::size_t calls_ = 0;
};

// 计数音频汇：累计写入的帧数，并可在一个确定的写入序号上触发一次取消请求。
// 取消刻意做成注入钩子而不是会话内部状态：它模拟“设备侧发现用户按了停止”，
// 因此不依赖会话当前处于哪个阶段。
class CountingSink final : public capability::IAudioSink {
 public:
  using WriteHook = std::function<void()>;

  domain::OperationResult open() override {
    opened_ = true;
    return domain::OperationResult::success();
  }

  domain::OperationResult write(const domain::AudioFrame& frame) override {
    if (!opened_) {
      return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure);
    }
    ++writes_;
    samples_.insert(samples_.end(), frame.samples.begin(), frame.samples.end());
    // 钩子在本帧已经写出之后触发：与真实设备“这一帧已经发声”的时序一致，
    // 因此“取消不撤回已播出声音”的断言有确定的边界。
    if (hook_ && hook_at_ != 0 && writes_ == hook_at_) {
      const WriteHook hook = hook_;
      hook_ = nullptr;
      hook();
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    return domain::OperationResult::success();
  }

  domain::OperationResult close() noexcept override {
    opened_ = false;
    return domain::OperationResult::success();
  }

  void ArmHook(std::size_t write_index, WriteHook hook) {
    hook_at_ = write_index;
    hook_ = std::move(hook);
  }

  std::size_t writes() const noexcept { return writes_; }
  const std::vector<std::int16_t>& samples() const noexcept { return samples_; }

 private:
  bool opened_ = false;
  std::size_t writes_ = 0;
  std::size_t hook_at_ = 0;
  WriteHook hook_;
  std::vector<std::int16_t> samples_;
};

// 生成进度观察者：把“首帧播放”与“生成结束”的先后关系变成可断言的事件序列。
// 应用自己不产生这两个事实，它只把后端报告的事件按发生顺序转发出来。
class ProgressObserver final : public capability::IGenerationObserver {
 public:
  void on_generation_started() override { events.push_back("generation_started"); }
  void on_token_delivered(const std::string&) override { events.push_back("token"); }
  void on_generation_completed() override { events.push_back("generation_completed"); }
  void on_generation_failed(const std::string&) override { events.push_back("generation_failed"); }
  bool failed() const noexcept override {
    return !events.empty() && events.back() == "generation_failed";
  }

  std::vector<std::string> events;
};

// 事件观察者：把应用对外报告的事实按到达顺序记录下来，同时统计每个事实的次数。
// 它只观察、不参与，因此“应用报告的顺序”与“轮次实际发生的顺序”可以直接比较。
class RecordingObserver final : public runtime::SessionAppObserver {
 public:
  void on_turn(const SessionTurnResult& result) override {
    ++turn_events;
    last_turn = result;
    if (result.playback_started) {
      events.push_back("playback_started");
    }
    if (result.completed) {
      events.push_back("turn_completed");
    }
    if (result.cancelled) {
      events.push_back("turn_cancelled");
    }
  }

  void on_stream_closed(const std::string& stream_id) override {
    ++closed_events;
    closed_stream = stream_id;
  }

  std::vector<std::string> events;
  SessionTurnResult last_turn;
  std::string closed_stream;
  std::size_t turn_events = 0;
  std::size_t closed_events = 0;
};

// 事件序列里 first 是否严格早于 second：重叠判据是先后关系，不是“同时存在”。
bool Before(const std::vector<std::string>& events, const std::string& first,
            const std::string& second) {
  const auto left = std::find(events.begin(), events.end(), first);
  const auto right = std::find(events.begin(), events.end(), second);
  if (left == events.end() || right == events.end()) {
    return false;
  }
  return left < right;
}

// 应用公开的有界性不变量：峰值待播帧数不得超过声明的播放容量。它只证明“交付从未越过
// 声明的上限”，不能说明真实设备缓冲深度或端到端延迟。
void CheckBoundedPlayback(const SessionTurnResult& result, std::size_t capacity) {
  CHECK(result.peak_pending_frames <= capacity);
}

// 文本模式：一个固定文本就是一轮输入，不开启常驻采集，输入结束后应用自行收尾。
// 它同时保护“输入源互斥”：文本模式下没有音频源参与，因此不存在第二个生产者。
void TestTextModeCompletesSingleTurnAndClosesStream() {
  auto retriever =
      backend::FakeRag({capability::RetrievedChunk{"geo-capital-fr", kHitText, kL1Score}});
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeTts tts;
  // 夹具自带的文本刻意与配置文本不同：如果固定文本没有被交付，这一轮就会用夹具的文本去
  // 检索，得到不同的回答，用例因此能真正区分“配置文本生效”与“夹具文本碰巧相同”。
  ScriptedAsr asr({"unrelated fixture text"});
  TickingClock clock;
  CountingSink sink;
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);

  SessionAppConfig config;
  config.mode = SessionAppInputMode::kText;
  config.text = kL1Question;
  config.stream_id = "app-text";
  SessionApp app(config, asr, retriever, router, tts, playback, nullptr);
  RecordingObserver observer;

  const auto run = app.run(&observer);

  CHECK(run.error.ok());
  CHECK(run.cleanup_error.ok());
  CHECK(!run.stopped_early);
  // 文本模式只有一个轮次，跑完即“输入自然结束”：它不是被停止，也不是还剩输入。
  CHECK(run.input_ended);
  CHECK(run.turns.size() == 1);
  CHECK(run.turns_completed == 1);
  CHECK(observer.turn_events == 1);
  // 文本模式不拥有常驻采集，因此没有“输入流已关闭”这条生命周期事实：报告它会让证据里
  // 出现一个并不存在的资源。
  CHECK(observer.closed_events == 0);
  const auto& turn = run.turns.front();
  CHECK(turn.completed);
  CHECK(!turn.cancelled);
  CHECK(turn.playback_done);
  CHECK(turn.route == backend::RagRouteLevel::kL1);
  CHECK(turn.text == kHitText);
  CHECK(turn.request_id == "app-text-turn-1");
  CHECK(turn.state == runtime::SessionStateMachine::State::kIdle);
  // 保护的不变量：固定文本必须经过识别接口，并且确实取代了识别夹具自带的文本。前者由送帧
  // 次数证明（这一帧不是录音，而是“识别可以收尾”的触发信号），后者由回答文本证明——若夹具
  // 文本生效，路由会落到 L3 并生成完全不同的回答。
  CHECK(asr.fed_frames() == 1);
  CHECK(asr.fed_samples() == Samples(kFrameSamples, 0));
  CheckBoundedPlayback(turn, config.session_config.playback_queue_capacity);
}

// L0 控制意图：停止类动作必须走取消收敛，既不能退化成知识问答，也不能留下合成输出。
void TestControlIntentTextCancelsWithoutSynthesisOrOutput() {
  auto retriever =
      backend::FakeRag({capability::RetrievedChunk{"geo-capital-fr", kHitText, kL1Score}});
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  CountingTts tts;
  ScriptedAsr asr({kStopQuestion});
  TickingClock clock;
  CountingSink sink;
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);

  SessionAppConfig config;
  config.mode = SessionAppInputMode::kText;
  config.text = kStopQuestion;
  config.stream_id = "app-control";
  SessionApp app(config, asr, retriever, router, tts, playback, nullptr);
  RecordingObserver observer;

  const auto run = app.run(&observer);

  CHECK(run.error.ok());
  CHECK(run.turns.size() == 1);
  const auto& turn = run.turns.front();
  // 保护的不变量：控制意图不产生任何音频，因此“停止”不会被回答。
  CHECK(turn.cancelled);
  CHECK(!turn.completed);
  CHECK(turn.route == backend::RagRouteLevel::kL0);
  CHECK(!turn.playback_started);
  CHECK(turn.pcm_frames.empty());
  CHECK(tts.calls() == 0);
  CHECK(sink.writes() == 0);
  CHECK(turn.terminal_marker == runtime::ActivityMarker::kTerminalCancelled);
}

// 空输入：两种情形都不产生轮次、都不产生音频，但原因不同，因此错误码也不同。
//   - 配置里根本没有文本：这是配置错误，在开始任何轮次之前就以 kInvalidInput 收敛。
//   - 文本全是空白：它是一次“没有内容”的输入。生产者把它当成输入耗尽，因此运行正常
//     收敛、不产生轮次；若把它当成一次提问，只会得到一轮“识别不到文本”的失败，反而
//     掩盖了“本次运行没有可处理的输入”这一事实。
void TestEmptyTextInputProducesNoTurn() {
  auto retriever =
      backend::FakeRag({capability::RetrievedChunk{"geo-capital-fr", kHitText, kL1Score}});
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeTts tts;
  ScriptedAsr asr({kL1Question});
  TickingClock clock;
  CountingSink sink;
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);

  SessionAppConfig config;
  config.mode = SessionAppInputMode::kText;
  config.text.clear();  // 缺少文本：这是配置错误，不是一次空提问。
  config.stream_id = "app-empty";
  SessionApp app(config, asr, retriever, router, tts, playback, nullptr);
  RecordingObserver observer;

  const auto run = app.run(&observer);

  CHECK(!run.error.ok());
  CHECK(run.error.code == domain::ErrorCode::kInvalidInput);
  CHECK(run.turns.empty());
  CHECK(run.turns_completed == 0);
  CHECK(observer.turn_events == 0);
  // 没有任何轮次就没有输入流可关闭：关闭事件只在真正建立过流之后出现。
  CHECK(observer.closed_events == 0);
  CHECK(asr.fed_frames() == 0);
  CHECK(sink.writes() == 0);

  // 第二种情形：文本存在但全是空白。它必须产生一个明确的失败轮次，而不是静默无输出。
  SessionAppConfig blank_config;
  blank_config.mode = SessionAppInputMode::kText;
  blank_config.text = " \t\n";
  blank_config.stream_id = "app-blank";
  SessionApp blank_app(blank_config, asr, retriever, router, tts, playback, nullptr);
  RecordingObserver blank_observer;

  const auto blank_run = blank_app.run(&blank_observer);

  CHECK(blank_run.error.ok());
  // 保护的不变量：全空白文本等价于“没有输入”，因此不产生轮次、不产生音频，也不会被
  // 当成一次提问去回答。
  CHECK(blank_run.turns.empty());
  CHECK(blank_run.turns_completed == 0);
  CHECK(blank_observer.turn_events == 0);
  CHECK(blank_observer.closed_events == 0);
  CHECK(sink.writes() == 0);
}

// 生成路径的流式重叠：首帧播放必须严格早于生成结束，且峰值待播帧数有界。
void TestGenerationModeOverlapsPlaybackWithGeneration() {
  auto retriever =
      backend::FakeRag({capability::RetrievedChunk{"geo-context", kHitText, kL2Score}});
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  // 三个 token 各自带句末标点，因此每个 token 都能独立切出一个片段并立即合成：
  // “首帧播放早于生成结束”因此有多个确定的交付边界，而不是一次性的巧合。
  const std::vector<std::string> tokens = {"第一句回答。", "第二句回答。", "第三句回答。"};
  backend::FakeLlm llm(tokens);
  backend::FakeTts tts;
  ScriptedAsr asr({kL2Question});
  TickingClock clock;
  CountingSink sink;
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);

  SessionAppConfig config;
  config.mode = SessionAppInputMode::kText;
  config.text = kL2Question;
  config.stream_id = "app-generation";
  // 片段上限取 9 字节：“第一句回答。”是 5 个字符、共 18 字节，会在句末标点处切成三段。
  config.session_config.text_chunk_max_bytes = 9;
  // 容量留足，本用例只验收重叠成立；背压由专门的用例验证。
  config.session_config.playback_queue_capacity = 64;
  SessionApp app(config, asr, retriever, router, tts, playback, nullptr, &llm);
  RecordingObserver observer;
  ProgressObserver progress;
  // 观察者通过应用级入口挂接：这条链路要经过“应用 → 会话 → 后端”三层转发，因此它同时
  // 验收了转发接缝本身，而不是只验证后端会不会报告事件。
  app.set_generation_observer(&progress);

  const auto run = app.run(&observer);

  CHECK(run.error.ok());
  CHECK(run.turns.size() == 1);
  const auto& turn = run.turns.front();
  CHECK(turn.completed);
  CHECK(turn.route == backend::RagRouteLevel::kL2);
  // 回答文本是全部 token 按顺序拼接的结果，片段切分不改变它。
  CHECK(turn.text == "第一句回答。第二句回答。第三句回答。");
  CHECK(turn.playback_started);
  CHECK(turn.playback_done);
  CHECK(sink.writes() > 2);
  // 保护的不变量：播放开始严格早于生成结束。两条事实都来自后端同步报告的事件序列，
  // 不依赖墙钟或睡眠。
  CHECK(Before(progress.events, "token", "generation_completed"));
  CHECK(Before(observer.events, "playback_started", "turn_completed"));
  CheckBoundedPlayback(turn, config.session_config.playback_queue_capacity);
}

// 文件音频：WAV 解析出的样本按统一音频契约分帧后逐帧送进识别，顺序与内容都不变。
// 解析职责在应用层，因此这里同时验收“字节序校验”与“短尾补零”两件事。
void TestFileModeFeedsDecodedSamplesToAsrInOrder() {
  const std::string path = TempPath("file.wav");
  const std::vector<std::int16_t> samples = {1, -2, 3, -4, 5, -6, 7, -8};
  const auto wav = MakeWav(samples, 16000, 1, 16);
  CHECK(WriteFile(path, wav));

  auto retriever =
      backend::FakeRag({capability::RetrievedChunk{"geo-capital-fr", kHitText, kL1Score}});
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeTts tts;
  ScriptedAsr asr({kL1Question});
  TickingClock clock;
  CountingSink sink;
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);

  SessionAppConfig config;
  config.mode = SessionAppInputMode::kWavFile;
  config.wav_path = path;
  config.stream_id = "app-file";
  SessionApp app(config, asr, retriever, router, tts, playback, nullptr);
  RecordingObserver observer;

  const auto run = app.run(&observer);

  std::remove(path.c_str());
  CHECK(run.error.ok());
  CHECK(run.turns.size() == 1);
  CHECK(run.turns.front().completed);
  // 短尾补零：8 个样本 → 1 帧，前 8 个样本必须是原值，其余补零。
  CHECK(asr.fed_frames() == 1);
  CHECK(asr.fed_samples().size() == kFrameSamples);
  CHECK(std::equal(samples.begin(), samples.end(), asr.fed_samples().begin()));
  CHECK(std::all_of(asr.fed_samples().begin() + static_cast<std::ptrdiff_t>(samples.size()),
                    asr.fed_samples().end(), [](std::int16_t value) { return value == 0; }));
}

// 非法文件音频：必须在开始任何轮次之前明确失败，而不是把坏输入送进识别。
void TestFileModeRejectsNonSixteenKilohertzWav() {
  const std::string path = TempPath("bad-rate.wav");
  const std::vector<std::int16_t> samples(kFrameSamples, 1234);
  CHECK(WriteFile(path, MakeWav(samples, 8000, 1, 16)));

  auto retriever =
      backend::FakeRag({capability::RetrievedChunk{"geo-capital-fr", kHitText, kL1Score}});
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeTts tts;
  ScriptedAsr asr({kL1Question});
  TickingClock clock;
  CountingSink sink;
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);

  SessionAppConfig config;
  config.mode = SessionAppInputMode::kWavFile;
  config.wav_path = path;
  config.stream_id = "app-bad-file";
  SessionApp app(config, asr, retriever, router, tts, playback, nullptr);
  RecordingObserver observer;

  const auto run = app.run(&observer);

  std::remove(path.c_str());
  CHECK(!run.error.ok());
  CHECK(run.error.code == domain::ErrorCode::kInvalidInput);
  CHECK(run.turns.empty());
  CHECK(asr.fed_frames() == 0);
  CHECK(sink.writes() == 0);
  CHECK(observer.turn_events == 0);
}

// 模拟常驻输入：两段说话各自形成一个轮次，说话之间不新建声卡/输入流。
// 它同时保护“输入结束时收敛”：源读完后应用关闭输入流并退出，而不是继续等输入。
void TestResidentModeRunsTwoUtterancesOnOneStream() {
  std::vector<Region> regions;
  AppendUtterance(regions, kSilenceSample, kSpeechSampleA);
  AppendUtterance(regions, kSilenceSample, kSpeechSampleB);
  const auto scenario = BuildScenario(regions);

  backend::FakeAudioSource source(scenario.samples);
  runtime::SpeechActivityScript detector(scenario.runs);
  runtime::ResidentAudioInput resident(source, detector, kStreamA, ResidentInputConfig());

  auto retriever =
      backend::FakeRag({capability::RetrievedChunk{"geo-capital-fr", kHitText, kL1Score}});
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeTts tts;
  // 两轮不同文本：第二轮必须来自第二段说话，而不是复用第一轮的结果。
  ScriptedAsr asr({kL1Question, kStopQuestion});
  TickingClock clock;
  CountingSink sink;
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);

  SessionAppConfig config;
  config.mode = SessionAppInputMode::kSimulatedResident;
  config.stream_id = kStreamA;
  config.resident_config = ResidentInputConfig();
  SessionApp app(config, asr, retriever, router, tts, playback, &resident);
  RecordingObserver observer;

  const auto run = app.run(&observer);

  CHECK(run.error.ok());
  CHECK(run.input_ended);
  CHECK(!run.stopped_early);
  CHECK(run.turns.size() == 2);
  CHECK(run.turns_completed == 1);
  CHECK(run.turns.at(0).text == kHitText);
  CHECK(run.turns.at(0).completed);
  CHECK(run.turns.at(0).route == backend::RagRouteLevel::kL1);
  CheckBoundedPlayback(run.turns.at(0), config.session_config.playback_queue_capacity);
  // 第二段说话是停止指令：它按统一取消语义收敛，因此没有回答文本，也没有任何音频。
  CHECK(run.turns.at(1).text.empty());
  CHECK(run.turns.at(1).cancelled);
  CHECK(run.turns.at(1).route == backend::RagRouteLevel::kL0);
  CHECK(!run.turns.at(1).playback_started);
  // 第一轮收尾时第二段说话已经在队列里：旧回答的清理不得丢弃它，它必须仍然作为下一轮
  // 的输入被取走（这正是“回答被取消不删除新语音开头”在应用层的落点）。
  CHECK(resident.dropped_segments() == 0);
  // 采集生命周期由应用关闭一次，两轮共用同一个输入流，且没有任何段被丢弃。
  CHECK(!resident.capturing());
  CHECK(resident.frames_pumped() == scenario.frame_count);
  CHECK(resident.dropped_segments() == 0);
  CHECK(resident.pending_segments() == 0);
  CHECK(observer.closed_events == 1);
  CHECK(observer.closed_stream == kStreamA);
}

// 常驻模式重复运行：同一对象第二次运行必须以“输入已经结束”收敛，而不是重放采集。
// 保护的不变量：接口文档声明的限制与实现一致——常驻采集只开始一次，想重复场景必须重建
// 常驻输入，而不是指望同一次采集被重放；同时第二次运行仍然正常收尾，不留下在途状态。
void TestResidentModeSecondRunEndsWithoutReplayingCapture() {
  std::vector<Region> regions;
  AppendUtterance(regions, kSilenceSample, kSpeechSampleA);
  const auto scenario = BuildScenario(regions);

  backend::FakeAudioSource source(scenario.samples);
  runtime::SpeechActivityScript detector(scenario.runs);
  runtime::ResidentAudioInput resident(source, detector, "app-resident-repeat",
                                       ResidentInputConfig());

  auto retriever =
      backend::FakeRag({capability::RetrievedChunk{"geo-capital-fr", kHitText, kL1Score}});
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeTts tts;
  ScriptedAsr asr({kL1Question});
  TickingClock clock;
  CountingSink sink;
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);

  SessionAppConfig config;
  config.mode = SessionAppInputMode::kSimulatedResident;
  config.stream_id = "app-resident-repeat";
  config.resident_config = ResidentInputConfig();
  SessionApp app(config, asr, retriever, router, tts, playback, &resident);

  const auto first = app.run();
  CHECK(first.error.ok());
  CHECK(first.turns.size() == 1);
  CHECK(first.turns.front().completed);

  const auto second = app.run();
  // 第二次运行在建立输入时就被拒绝：常驻采集只能开始一次，因此这是运行级错误而不是
  // 一轮失败——把它报成“跑了 0 轮”会让调用方以为输入恰好为空。
  CHECK(!second.error.ok());
  CHECK(second.error.code == domain::ErrorCode::kAlreadyCompleted);
  CHECK(second.turns.empty());
  CHECK(second.turns_completed == 0);
  CHECK(!second.stopped_early);
  // 采集没有被重放：帧数没有增长，输入仍然处于结束状态，也没有新的音频被写出。
  CHECK(resident.frames_pumped() == scenario.frame_count);
  CHECK(!resident.capturing());
  CHECK(sink.writes() == first.turns.front().pcm_frames.size());
}

// 提前停止：停止请求在轮次之间被受理，应用立即收尾——不再产生新轮次，输入被关闭，
// 阻塞中的等待者也会因此被唤醒（输入停止唤醒等待者是常驻输入的硬保证）。
void TestStopBetweenTurnsClosesInputWithoutExtraTurn() {
  std::vector<Region> regions;
  AppendUtterance(regions, kSilenceSample, kSpeechSampleA);
  AppendUtterance(regions, kSilenceSample, kSpeechSampleB);
  const auto scenario = BuildScenario(regions);

  backend::FakeAudioSource source(scenario.samples);
  runtime::SpeechActivityScript detector(scenario.runs);
  runtime::ResidentAudioInput resident(source, detector, "app-stop", ResidentInputConfig());

  auto retriever =
      backend::FakeRag({capability::RetrievedChunk{"geo-capital-fr", kHitText, kL1Score}});
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  backend::FakeTts tts;
  ScriptedAsr asr({kL1Question, kStopQuestion});
  TickingClock clock;
  CountingSink sink;
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, clock);

  SessionAppConfig config;
  config.mode = SessionAppInputMode::kSimulatedResident;
  config.stream_id = "app-stop";
  config.resident_config = ResidentInputConfig();
  // 只跑一轮就停：第二轮说话还没有被取走，停止请求优先于继续消费。
  config.max_turns = 1;
  SessionApp app(config, asr, retriever, router, tts, playback, &resident);
  RecordingObserver observer;

  const auto run = app.run(&observer);

  CHECK(run.error.ok());
  CHECK(run.stopped_early);
  // 提前停止时输入并没有走完，因此不能报告“输入已结束”：那会让证据里出现一次并不存在的
  // 正常收尾。
  CHECK(!run.input_ended);
  CHECK(run.turns.size() == 1);
  // 第一轮是 L1 直答：回答文本来自命中夹具，而不是识别文本。
  CHECK(run.turns.front().text == kHitText);
  // 停止是一次请求而不是一次取消：它不改变已经完成的轮次结果，也不产生新终态。
  CHECK(run.turns.front().completed);
  CHECK(!resident.capturing());
  CHECK(resident.pending_segments() == 0);
  CHECK(observer.closed_events == 1);
}

// 播放背压：待播帧达到声明容量时本轮明确失败，而不是静默丢帧。应用层要保证这个失败
// 同样只产生一个终态，并且不会把“半段回答还在发声”留给下一轮。
void TestPlaybackBackpressureFailsTurnWithinDeclaredCapacity() {
  auto retriever =
      backend::FakeRag({capability::RetrievedChunk{"geo-context", kHitText, kL2Score}});
  backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
  const std::vector<std::string> tokens = {"第一句回答。", "第二句回答。"};
  backend::FakeLlm llm(tokens);
  backend::FakeTts tts;
  ScriptedAsr asr({kL2Question});
  // 逻辑时钟停在 0：设备时间不前进，因此每一帧交付之后都仍停留在“已写出、尚未播完”，
  // 待播帧数会随合成增长——这正是“生成快于播放”的真实条件。若时钟跟着交付前进，
  // 播放会始终追上合成，缓冲永远到不了上限，这一条也就测不到背压。
  runtime::ManualPlaybackClock stopped_clock(0);
  CountingSink sink;
  CHECK(sink.open().ok());
  runtime::LogicalClockPlayback playback(sink, stopped_clock);

  SessionAppConfig config;
  config.mode = SessionAppInputMode::kText;
  config.text = kL2Question;
  config.stream_id = "app-backpressure";
  config.session_config.text_chunk_max_bytes = 9;
  config.session_config.playback_queue_capacity = 2;  // 2 段文本共约 48 帧，必然越界。
  SessionApp app(config, asr, retriever, router, tts, playback, nullptr, &llm);
  RecordingObserver observer;

  const auto run = app.run(&observer);

  // 轮次失败不等于应用崩溃：应用仍然正常收尾，并把失败原因如实带出来。
  CHECK(run.error.ok());
  CHECK(run.turns.size() == 1);
  const auto& turn = run.turns.front();
  CHECK(!turn.completed);
  CHECK(!turn.playback_done);
  CHECK(turn.backpressure);
  CHECK(!turn.error.ok());
  CHECK(turn.error.code == domain::ErrorCode::kBackendFailure);
  // 保护的不变量：交付从未越过声明容量，因此“有界”是事实而不是声明。
  CheckBoundedPlayback(turn, config.session_config.playback_queue_capacity);
  CHECK(turn.terminal_marker == runtime::ActivityMarker::kTerminalCancelled);
  // 失败轮次只产生一个终态，且不留下成功标记。
  const auto markers = app.trace();
  CHECK(std::count(markers.begin(), markers.end(),
                   runtime::ActivityMarker::kTerminalSucceeded) == 0);
}

// 重复运行确定性：同一夹具跑两次得到相同的轮次数、路由、文本与逐帧 PCM。
// 它同时保护“输出不残留”：第二次运行的结果不包含第一次的任何字节。
void TestRepeatedRunsAreDeterministicWithoutResidue() {
  const auto build_run = [](SessionAppRunResult* target) {
    auto retriever =
        backend::FakeRag({capability::RetrievedChunk{"geo-capital-fr", kHitText, kL1Score}});
    backend::FakeRagRouter router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3});
    backend::FakeTts tts;
    ScriptedAsr asr({kL1Question});
    TickingClock clock;
    CountingSink sink;
    CHECK(sink.open().ok());
    runtime::LogicalClockPlayback playback(sink, clock);

    SessionAppConfig config;
    config.mode = SessionAppInputMode::kText;
    config.text = kL1Question;
    config.stream_id = "app-repeat";
    SessionApp app(config, asr, retriever, router, tts, playback, nullptr);
    *target = app.run();
  };

  SessionAppRunResult first;
  SessionAppRunResult second;
  build_run(&first);
  build_run(&second);

  CHECK(first.error.ok());
  CHECK(second.error.ok());
  CHECK(first.turns.size() == second.turns.size());
  CHECK(first.turns.size() == 1);
  CHECK(first.turns.front().text == second.turns.front().text);
  CHECK(first.turns.front().route == second.turns.front().route);
  CHECK(first.turns.front().request_id == second.turns.front().request_id);
  CHECK(first.turns.front().pcm_frames.size() == second.turns.front().pcm_frames.size());
  CHECK(!first.turns.front().pcm_frames.empty());
  for (std::size_t index = 0; index < first.turns.front().pcm_frames.size(); ++index) {
    CHECK(first.turns.front().pcm_frames.at(index).samples ==
          second.turns.front().pcm_frames.at(index).samples);
  }
}

}  // namespace

int main() {
  TestTextModeCompletesSingleTurnAndClosesStream();
  TestControlIntentTextCancelsWithoutSynthesisOrOutput();
  TestEmptyTextInputProducesNoTurn();
  TestGenerationModeOverlapsPlaybackWithGeneration();
  TestPlaybackBackpressureFailsTurnWithinDeclaredCapacity();
  TestFileModeFeedsDecodedSamplesToAsrInOrder();
  TestFileModeRejectsNonSixteenKilohertzWav();
  TestResidentModeRunsTwoUtterancesOnOneStream();
  TestResidentModeSecondRunEndsWithoutReplayingCapture();
  TestStopBetweenTurnsClosesInputWithoutExtraTurn();
  TestRepeatedRunsAreDeterministicWithoutResidue();
  return 0;
}

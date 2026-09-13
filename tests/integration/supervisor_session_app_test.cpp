// 进程内会话拥有者的集成夹具：验收 Supervisor 与单进程 Session 应用接起来之后的行为。
//
// 职责：监督器自身的生命周期语义由 supervisor_lifecycle_test.cpp 用闸门夹具精确验收；
// 本文件只回答另一个问题——把真实的会话应用挂到那条接缝上，一次会话能不能被完整跑完、
// 身份能不能归位、失败能不能被如实记账、多次交互与重复运行会不会互相污染。两者分工明确，
// 避免同一批断言在两处互相掩盖。
//
// 保护的不变量（每个用例在断言旁注明它保护哪一条）：
//   1. 接缝可用：监督器创建并驱动一个真实的会话应用跑完整条链路（识别 → 路由 → 直答 →
//      合成 → 播放），会话正常收敛后槽位释放，记账为一次完成。
//   2. 身份归位：每次会话的运行记录带着当次启动身份（含监督器分配的会话序号），连续两次
//      会话各自归位，不会互相覆盖成同一条。
//   3. 建立失败不产生会话：配置非法在“建立”阶段就被拒绝，会话应用一次都没有执行，
//      因此不会有半途而废的轮次或残留输出。
//   4. 运行级失败如实上报：会话应用报告运行级错误时，监督器把它记账为一次失败会话，
//      但槽位仍然在清理成功后回到可用——失败的是这一次会话，不是这台设备。
//   5. 会话是多次交互的容器：一次受监督的会话内部可以发生多轮交互（这里是两段说话），
//      它们共享同一个会话身份，不需要也不允许为“再来一次”新建会话。
//   6. 无残留与确定性：上一次会话的结果不会漏进下一次（失败的那次尤其明显），
//      而同一输入重复运行时逐轮结果一致。
//
// 夹具全部确定性：播放节奏由逻辑时钟推进，输入是固定的短文本、固定样本的 WAV 或固定活动
// 序列，不引入模型、声卡、网络或真实时间等待；唯一的外部资源是本文件自己创建并删除的临时
// WAV 文件。
#include "../test_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "fake_audio.hpp"
#include "fake_rag.hpp"
#include "fake_tts.hpp"
#include "resident_audio_input.hpp"
#include "session_app_owner.hpp"
#include "speech_segmentation.hpp"
#include "supervisor.hpp"

using namespace nexweave;
using nexweave::domain::ErrorCode;
using nexweave::runtime::SessionAppConfig;
using nexweave::runtime::SessionAppInputMode;
using nexweave::runtime::SessionAppOwnerFactory;
using nexweave::runtime::SessionAppRunRecord;
using nexweave::runtime::SessionAppRunResult;
using nexweave::runtime::Supervisor;
using nexweave::runtime::SupervisorConfig;
using nexweave::runtime::SupervisorSessionSpec;
using nexweave::runtime::SupervisorState;

namespace {

using Milliseconds = std::chrono::milliseconds;

// 会话收敛的等待上界。夹具跑得极快，等满即说明实现有缺陷而不是机器慢。
constexpr Milliseconds kWaitBound{4000};

// 命中夹具：分数 0.90 使问句走 L1 直答，因此整条链路不需要 LLM。
constexpr double kL1Score = 0.90;
constexpr char kHitText[] = "the capital of france is paris";
// 问句必须是命中文本的子串（确定性检索器的匹配方向是“片段文本包含查询”）。
constexpr char kL1Question[] = "the capital of france";
// 文本模式下的识别替身文本：它故意与配置文本不同，使“配置文本真的生效了”成为可区分的事实
// ——这条文本命中不了任何片段，只会走 L3 并在没有 LLM 时失败。
constexpr char kUnrelatedAsrText[] = "unrelated fixture text";

// 统一音频契约的固定帧长：1 帧 = 320 样本 = 20 ms。
constexpr std::size_t kFrameSamples = domain::kAudioFrameSamples;

// 播放逻辑时钟：每次读取前进一个帧时长，使交付的每一帧都立即结算为“已播放”。
// 刻度与读取次数绑定，因此“合成结束后确实播完”在毫秒级预算内即可成立，不引入墙钟或睡眠。
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

// 识别替身：记录送帧次数，并在每次结束帧交付构造时给定的文本。文本模式与音频模式交付的
// 内容不同，因此两种模式下“这一轮的识别结果从哪来”都可以被区分。
class FixtureAsr final : public capability::IAsr {
 public:
  explicit FixtureAsr(std::string final_text) : final_text_(std::move(final_text)) {}

  domain::OperationResult set_callback(capability::TextEventCallback callback) override {
    if (!callback) {
      return domain::OperationResult::failure(ErrorCode::kInvalidInput);
    }
    callback_ = std::move(callback);
    return domain::OperationResult::success();
  }

  domain::OperationResult feed(const domain::AudioFrame& /*frame*/, bool is_last) override {
    if (!callback_) {
      return domain::OperationResult::failure(ErrorCode::kInvalidInput);
    }
    ++fed_frames_;
    if (is_last) {
      callback_(capability::TextEvent{capability::TextEventKind::kFinal, final_text_, {}});
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    return domain::OperationResult::success();
  }

  int fed_frames() const noexcept {
    return fed_frames_;
  }

 private:
  std::string final_text_;
  capability::TextEventCallback callback_;
  int fed_frames_ = 0;
};

// 活动判定替身：全零帧算静音，含非零样本的帧算人声。它不识别波形，只把夹具构造的
// “静音/人声”模式变回逐帧判定，因此常驻输入在没有真实 VAD 的情况下也能确定性切段。
// 判定结果用 Result 表达：判定失败与“判定为静音”必须可区分，否则夹具会把一次失败
// 伪装成静音，静默截断用户的话。
class AmplitudeActivityDetector final : public runtime::ISpeechActivityDetector {
 public:
  domain::Result<runtime::SpeechActivity> detect(const domain::AudioFrame& frame) override {
    for (const auto sample : frame.samples) {
      if (sample != 0) {
        return domain::Result<runtime::SpeechActivity>::success(
            runtime::SpeechActivity::kSpeech);
      }
    }
    return domain::Result<runtime::SpeechActivity>::success(runtime::SpeechActivity::kSilence);
  }

  // 本替身不保留跨帧状态，复位因此是空操作；接口要求它必须是显式可调用的。
  void reset() override {}
};

// 夹具集合：能力对象必须比工厂活得久，因此把它们收在一个结构里，由用例在栈上按
// “夹具 → 工厂 → 监督器”的逆序析构，避免任何借用悬空。
struct Fixture {
  backend::FakeRag retriever;
  backend::FakeRagRouter router;
  backend::FakeTts tts;
  FixtureAsr asr;
  AmplitudeActivityDetector detector;
  TickingClock clock;
  backend::FakeAudioSink sink;
  runtime::LogicalClockPlayback playback;

  explicit Fixture(std::string asr_text = kUnrelatedAsrText)
      : retriever({capability::RetrievedChunk{"geo-capital-fr", kHitText, kL1Score}}),
        router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3}),
        asr(std::move(asr_text)),
        playback(sink, clock) {
    // 汇必须先打开：未打开的写入是设备错误，而不是被忽略的调用。播放组件不负责开关设备
    // （见 IAudioPlayback 的约定），因此“谁创建谁打开”落在这里。
    CHECK(sink.open().ok());
  }
};

SessionAppConfig TextModeConfig() {
  SessionAppConfig config;
  config.mode = SessionAppInputMode::kText;
  config.text = kL1Question;
  config.stream_id = "supervisor-session-app";
  return config;
}

SessionAppConfig WavModeConfig(const std::string& path) {
  SessionAppConfig config;
  config.mode = SessionAppInputMode::kWavFile;
  config.wav_path = path;
  config.stream_id = "supervisor-session-app";
  return config;
}

// 常驻模式的分段策略：一次说话 8 帧、静音 6 帧超时，因此“静音 4 + 人声 8 + 静音 6”共 18 帧
// 交付一个段。取值刻意取小，使整条用例只有几十帧，不需要真实时间。
runtime::ResidentAudioInputConfig ResidentConfig() {
  runtime::ResidentAudioInputConfig config;
  config.segmentation.preroll_frames = 2;
  config.segmentation.min_silence_frames = 6;
  config.segmentation.min_speech_frames = 4;
  config.segmentation.max_speech_frames = 40;
  config.pending_segment_capacity = 4;
  return config;
}

// 两段说话的固定活动序列：每段“静音 4 帧 + 人声 8 帧 + 静音 6 帧”。人声用非零样本，
// 静音用全零样本，因此判定替身可以只按幅度区分，而无需任何波形处理。
std::vector<std::int16_t> TwoUtteranceSamples() {
  std::vector<std::int16_t> samples;
  for (int utterance = 0; utterance < 2; ++utterance) {
    samples.insert(samples.end(), 4 * kFrameSamples, 0);
    samples.insert(samples.end(), 8 * kFrameSamples, 4000);
    samples.insert(samples.end(), 6 * kFrameSamples, 0);
  }
  return samples;
}

SupervisorConfig FastConfig() {
  SupervisorConfig config;
  config.start_wait_budget = kWaitBound;
  config.cleanup_wait_budget = kWaitBound;
  return config;
}

SupervisorSessionSpec MakeSpec(const std::string& work_id) {
  SupervisorSessionSpec spec;
  spec.work_id = work_id;
  spec.session_id = "sess-" + work_id;
  spec.request_id = "req-" + work_id;
  return spec;
}

// ---- 临时 WAV 夹具 ----

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

// 最小合法 RIFF/WAVE 文件（16 kHz、单声道、16 位 PCM）。样本数必须是整帧，否则应用会在
// 开始任何轮次之前以其分帧契约拒绝它。
std::vector<std::uint8_t> MakeWav(const std::vector<std::int16_t>& samples) {
  const std::uint32_t data_bytes =
      static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
  std::vector<std::uint8_t> wav;
  AppendAscii(wav, "RIFF");
  AppendLe32(wav, 36U + data_bytes);
  AppendAscii(wav, "WAVE");
  AppendAscii(wav, "fmt ");
  AppendLe32(wav, 16);
  AppendLe16(wav, 1);  // PCM
  AppendLe16(wav, 1);  // 单声道
  AppendLe32(wav, 16000);
  AppendLe32(wav, 16000 * 2);
  AppendLe16(wav, 2);
  AppendLe16(wav, 16);
  AppendAscii(wav, "data");
  AppendLe32(wav, data_bytes);
  for (const auto value : samples) {
    AppendLe16(wav, static_cast<std::uint16_t>(value));
  }
  return wav;
}

// 临时文件路径：带进程号，因此同一个测试进程内的固定名字不会与并发运行的同名测试互撞。
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

// ---- 用例 ----

// 保护不变量 1、2：监督器能把一个真实的会话应用完整跑完并释放槽位；运行记录带着当次
// 启动身份，走的是 L1 直答（因此不需要 LLM），文本取自配置而不是识别夹具。
void TestSupervisorRunsSessionAppToCompletion() {
  Fixture fixture;
  SessionAppOwnerFactory factory(TextModeConfig(), fixture.asr, fixture.retriever, fixture.router,
                                 fixture.tts, fixture.playback);
  Supervisor supervisor(factory, FastConfig());

  const auto started = supervisor.start(MakeSpec("work-l1"));
  CHECK(started.ok());
  CHECK(supervisor.wait_for_slot(kWaitBound));

  const runtime::SupervisorStatus status = supervisor.status();
  CHECK(status.state == SupervisorState::kIdle);
  CHECK(status.slot_reusable());
  CHECK(status.sessions_started == 1);
  CHECK(status.sessions_completed == 1);
  CHECK(status.sessions_failed == 0);
  CHECK(status.last_run_error.ok());
  CHECK(status.last_cleanup_error.ok());

  const std::shared_ptr<const SessionAppRunRecord> record = factory.last_run();
  CHECK(record != nullptr);
  CHECK(record->spec.work_id == "work-l1");
  CHECK(record->spec.session_sequence == 1);
  CHECK(record->spec.session_id == "sess-work-l1");
  CHECK(record->spec.request_id == "req-work-l1");

  const SessionAppRunResult& run = record->result;
  CHECK(run.error.ok());
  CHECK(run.turns.size() == 1);
  CHECK(run.turns_completed == 1);
  CHECK(run.turns_failed == 0);
  // 配置文本确实生效：只有把问句交给检索器才可能命中并走 L1 直答。识别夹具交付的是
  // “unrelated fixture text”，它命中不了任何片段，只会走 L3 并在没有 LLM 时失败——
  // 因此“路由是 L1”本身就是“固定文本来自配置”的可区分证据。
  CHECK(run.turns.front().route == backend::RagRouteLevel::kL1);
  CHECK(run.turns.front().text == kHitText);  // L1 的回答就是命中片段文本
  CHECK(run.turns.front().text != kUnrelatedAsrText);
  CHECK(run.turns.front().completed);
  CHECK(!run.turns.front().pcm_frames.empty());  // 直答确实被合成并交付播放
  CHECK(fixture.asr.fed_frames() == 1);          // 文本模式用一帧静音驱动“识别完成”
}

// 保护不变量 2：连续两次会话各自归位；第二次带着更高的会话序号与自己的身份，
// 运行记录是“最近一次”的不可变快照，不是两次混在一起。
void TestSequentialSessionsCarryTheirOwnIdentity() {
  Fixture fixture;
  SessionAppOwnerFactory factory(TextModeConfig(), fixture.asr, fixture.retriever, fixture.router,
                                 fixture.tts, fixture.playback);
  Supervisor supervisor(factory, FastConfig());

  CHECK(supervisor.start(MakeSpec("work-first")).ok());
  CHECK(supervisor.wait_for_slot(kWaitBound));
  const std::shared_ptr<const SessionAppRunRecord> first = factory.last_run();
  CHECK(first != nullptr);
  CHECK(first->spec.work_id == "work-first");
  CHECK(first->spec.session_sequence == 1);

  CHECK(supervisor.start(MakeSpec("work-second")).ok());
  CHECK(supervisor.wait_for_slot(kWaitBound));
  const std::shared_ptr<const SessionAppRunRecord> second = factory.last_run();
  CHECK(second != nullptr);
  CHECK(second->spec.work_id == "work-second");
  CHECK(second->spec.session_sequence == 2);
  CHECK(second->result.error.ok());
  CHECK(second->result.turns_completed == 1);

  const runtime::SupervisorStatus status = supervisor.status();
  CHECK(status.session_sequence == 2);
  CHECK(status.sessions_started == 2);
  CHECK(status.sessions_completed == 2);
  CHECK(status.sessions_failed == 0);
  // 第一次会话的快照不会被第二次改写：它是不可变记录，而不是会翻页的视图。
  CHECK(first->spec.session_sequence == 1);
  CHECK(first->spec.work_id == "work-first");
}

// 保护不变量 5：一次受监督的会话可以承载多轮交互。两段说话在同一个会话里跑完，共享同一个
// 会话身份与序号——这正是“再来一次”不需要新建会话的原因，也是语音打断只能在会话内部按
// 代际切换、而不能靠并发创建绕过单活跃约束的结构前提。
void TestMultipleUtterancesShareOneSupervisorSession() {
  Fixture fixture(kL1Question);
  const runtime::ResidentAudioInputConfig resident_config = ResidentConfig();
  backend::FakeAudioSource source(TwoUtteranceSamples());
  runtime::ResidentAudioInput resident(source, fixture.detector, "supervisor-resident",
                                       resident_config);

  SessionAppConfig config;
  config.mode = SessionAppInputMode::kSimulatedResident;
  config.stream_id = "supervisor-resident";
  config.resident_config = resident_config;

  SessionAppOwnerFactory factory(config, fixture.asr, fixture.retriever, fixture.router,
                                 fixture.tts, fixture.playback, &resident);
  Supervisor supervisor(factory, FastConfig());

  CHECK(supervisor.start(MakeSpec("work-two-turns")).ok());
  CHECK(supervisor.wait_for_slot(kWaitBound));

  const runtime::SupervisorStatus status = supervisor.status();
  CHECK(status.sessions_started == 1);
  CHECK(status.sessions_completed == 1);
  CHECK(status.session_sequence == 1);  // 两轮交互只消耗一个会话身份

  const std::shared_ptr<const SessionAppRunRecord> record = factory.last_run();
  CHECK(record != nullptr);
  const SessionAppRunResult& result = record->result;
  CHECK(result.error.ok());
  CHECK(result.turns.size() == 2);  // 两段说话各成一轮
  CHECK(result.turns_completed == 2);
  CHECK(result.segments_queued == 2);
  CHECK(result.turns.front().route == backend::RagRouteLevel::kL1);
  CHECK(result.turns.back().route == backend::RagRouteLevel::kL1);
  CHECK(result.turns.front().text == kHitText);
  CHECK(result.turns.back().text == kHitText);
}

// 保护不变量 3：配置非法在建立阶段被拒绝——会话应用一次都没有执行，因此没有轮次、
// 没有输出，也不会把“参数不对”变成一个已经开始的会话。
void TestInvalidConfigFailsAtStartWithoutRunningSession() {
  Fixture fixture;
  SessionAppConfig config = TextModeConfig();
  config.text.clear();  // 文本模式必须有非空文本，否则没有输入可交付
  SessionAppOwnerFactory factory(config, fixture.asr, fixture.retriever, fixture.router,
                                 fixture.tts, fixture.playback);
  Supervisor supervisor(factory, FastConfig());

  const auto started = supervisor.start(MakeSpec("work-invalid"));
  CHECK(!started.ok());
  CHECK(started.error.code == ErrorCode::kInvalidInput);

  const runtime::SupervisorStatus status = supervisor.status();
  CHECK(status.state == SupervisorState::kIdle);  // 建立失败已回滚，槽位没有留下占用
  CHECK(status.slot_reusable());
  CHECK(status.sessions_started == 0);
  CHECK(status.sessions_failed == 1);
  CHECK(status.last_start_error.code == ErrorCode::kInvalidInput);
  CHECK(fixture.asr.fed_frames() == 0);      // 没有一帧进入识别，也就没有会话被执行
  CHECK(factory.last_run() == nullptr);      // 没有任何会话收敛，因此没有运行记录
}

// 保护不变量 4、6：同一个监督器连续跑三次会话——成功、因输入消失而运行级失败、再次成功。
// 失败的会话不得留下上一次的轮次；再次成功的会话必须与第一次逐轮一致（确定性）。
void TestFailuresLeaveNoResidueAndRerunsAreDeterministic() {
  const std::string path = TempPath("supervisor-fixture.wav");
  const std::vector<std::uint8_t> wav = MakeWav(std::vector<std::int16_t>(2 * kFrameSamples, 1200));
  CHECK(WriteFile(path, wav));

  Fixture fixture(kL1Question);
  SessionAppOwnerFactory factory(WavModeConfig(path), fixture.asr, fixture.retriever,
                                 fixture.router, fixture.tts, fixture.playback);
  Supervisor supervisor(factory, FastConfig());

  // 第一次：输入可读，完成一轮。
  CHECK(supervisor.start(MakeSpec("work-run-1")).ok());
  CHECK(supervisor.wait_for_slot(kWaitBound));
  const std::shared_ptr<const SessionAppRunRecord> first = factory.last_run();
  CHECK(first != nullptr);
  CHECK(first->result.error.ok());
  CHECK(first->result.turns.size() == 1);
  CHECK(first->result.turns_completed == 1);
  CHECK(first->result.turns.front().text == kHitText);
  const std::size_t first_frames = first->result.turns.front().pcm_frames.size();
  CHECK(first_frames > 0);

  // 第二次：把输入文件删掉。取不到输入是**运行级**失败，因此不产生任何轮次——上一次的轮次
  // 不能作为残留出现在这一次的结果里。
  CHECK(std::remove(path.c_str()) == 0);
  CHECK(supervisor.start(MakeSpec("work-run-2")).ok());
  CHECK(supervisor.wait_for_slot(kWaitBound));
  const std::shared_ptr<const SessionAppRunRecord> second = factory.last_run();
  CHECK(second != nullptr);
  CHECK(second->spec.session_sequence == 2);
  CHECK(!second->result.error.ok());
  CHECK(second->result.turns.empty());
  CHECK(second->result.turns_completed == 0);
  const runtime::SupervisorStatus failed = supervisor.status();
  CHECK(failed.state == SupervisorState::kIdle);  // 运行级失败不影响槽位复用
  CHECK(failed.sessions_started == 2);
  CHECK(failed.sessions_completed == 1);
  CHECK(failed.sessions_failed == 1);

  // 第三次：把同一个文件写回来。同一输入重复运行必须得到逐轮一致的结果。
  CHECK(WriteFile(path, wav));
  CHECK(supervisor.start(MakeSpec("work-run-3")).ok());
  CHECK(supervisor.wait_for_slot(kWaitBound));
  const std::shared_ptr<const SessionAppRunRecord> third = factory.last_run();
  CHECK(third != nullptr);
  CHECK(third->spec.session_sequence == 3);
  CHECK(third->result.error.ok());
  CHECK(third->result.turns.size() == 1);
  CHECK(third->result.turns.front().route == first->result.turns.front().route);
  CHECK(third->result.turns.front().text == first->result.turns.front().text);
  CHECK(third->result.turns.front().pcm_frames.size() == first_frames);
  for (std::size_t index = 0; index < first_frames; ++index) {
    CHECK(third->result.turns.front().pcm_frames.at(index).samples ==
          first->result.turns.front().pcm_frames.at(index).samples);
  }
  CHECK(supervisor.status().sessions_completed == 2);

  CHECK(std::remove(path.c_str()) == 0);
}

}  // namespace

int main() {
  try {
    TestSupervisorRunsSessionAppToCompletion();
    TestSequentialSessionsCarryTheirOwnIdentity();
    TestMultipleUtterancesShareOneSupervisorSession();
    TestInvalidConfigFailsAtStartWithoutRunningSession();
    TestFailuresLeaveNoResidueAndRerunsAreDeterministic();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "监督器与会话应用集成用例未通过: %s\n", error.what());
    return 1;
  }
  return 0;
}

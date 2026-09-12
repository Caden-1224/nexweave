// 单进程 Session 应用的可执行入口。
//
// 职责：把命令行参数变成一次确定性的会话运行——选择输入模式（文本 / 固定文件音频 /
// 模拟常驻输入）、组装能力夹具、驱动 SessionApp、打印结果与可选的运行事件。
// 它不实现任何模型、设备或协议：默认全部使用确定性夹具，因此同一条命令重复运行得到
// 相同结果，可以在没有硬件、没有网络的机器上复现。
//
// 进程模型与退出路径：
//   - 输入、执行与播放全部在本进程的主线程上串行推进，因此不存在“后台工作线程还在跑”
//     的收尾问题；唯一的异步来源是信号。
//   - SIGINT/SIGTERM 的处理函数只做一次原子置位，然后立即返回；实际收敛发生在应用的下一个
//     轮次边界。常驻模式下一次取段的泵入预算是有限的，因此“设备既不产出也不结束”的输入
//     也不会让进程无法退出。
//   - 退出码：按契约收敛（含被停止、被取消、被背压打断的单轮）为 0；配置错误、读文件失败、
//     路由/合成失败或取段失败为 1。区分标准是“这次运行是否正常结束”，而不是“是否每一轮
//     都成功”——取消是正常语义，不是故障。
//
// 输出：默认打印人可读摘要；--events 额外输出逐轮事件 JSONL，--metrics 输出逐轮指标
// JSONL，--summary-json 输出一份机器可读的汇总。这些输出都不含时间戳，因此可以直接用于
// 逐字节比较的确定性验证；时间戳与运行清单属于后续的运行证据任务。
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "fake_asr.hpp"
#include "fake_audio.hpp"
#include "fake_llm.hpp"
#include "fake_rag.hpp"
#include "fake_tts.hpp"
#include "observability.hpp"
#include "session_app.hpp"
#include "session_runtime.hpp"
#include "speech_segmentation.hpp"

namespace {

using nexweave::backend::FakeAudioSink;
using nexweave::backend::FakeAudioSource;
using nexweave::backend::FakeLlm;
using nexweave::backend::FakeRag;
using nexweave::backend::FakeRagRouter;
using nexweave::backend::FakeTts;
using nexweave::backend::RagRouteLevel;
using nexweave::domain::AudioFrame;
using nexweave::capability::RetrievedChunk;
using nexweave::capability::TextEvent;
using nexweave::capability::TextEventCallback;
using nexweave::capability::TextEventKind;
using nexweave::domain::ErrorCode;
using nexweave::domain::OperationResult;
using nexweave::runtime::ActivityRun;
using nexweave::runtime::IPlaybackClock;
using nexweave::runtime::LogicalClockPlayback;
using nexweave::runtime::ResidentAudioInput;
using nexweave::runtime::SessionApp;
using nexweave::runtime::SessionAppConfig;
using nexweave::runtime::SessionAppInputMode;
using nexweave::runtime::SessionAppObserver;
using nexweave::runtime::SessionStateMachine;
using nexweave::runtime::SessionTurnResult;
using nexweave::runtime::SpeechActivity;
using nexweave::runtime::SpeechActivityScript;

constexpr std::size_t kFrameSamples = nexweave::domain::kAudioFrameSamples;
constexpr std::size_t kFrameMs = nexweave::domain::kAudioFrameDurationMs;

// 信号处理只能触达无锁、可重入的状态：应用对象不是信号安全的，因此处理函数只做两件事——
// 置位 sig_atomic_t 供 main 判断“本次收敛是否由信号触发”，以及请求应用停止（一次原子置位）。
// 真正的收敛发生在主循环的下一个轮次边界，而不是在信号处理函数里。
volatile std::sig_atomic_t g_signal = 0;
SessionApp* g_app = nullptr;

void handle_signal(int /*signal*/) {
  g_signal = 1;
  if (g_app != nullptr) {
    g_app->request_stop();
  }
}

// 回放节奏：真实墙钟。每个 20 ms 帧覆盖 20 ms 的墙钟时间，因此“合成结束但还没播完”是
// 一件真实可能发生的事。advance 对墙钟没有意义，由基类的默认实现承担。
class SteadyPlaybackClock final : public IPlaybackClock {
 public:
  std::int64_t now_ms() const noexcept override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // 真实时间不能由调用方推进：显式实现为空操作而不是沿用隐含约定，让“这个时钟不接受
  // 外部拨动”成为可读事实。
  void advance(std::int64_t /*delta_ms*/) override {}
};

// 设备侧取消夹具：在第 N 帧已经写出之后请求取消本轮。它模拟“用户在播放中途按了停止”，
// 触发点是能力契约里允许的唯一重入位置（投递回调内），因此不需要额外线程。取消发生在
// 帧已经写出之后，与“已播出的声音不可撤回”的时序一致。
// 与应用对象的绑定发生在本对象构造之后：应用需要一个播放组件引用才能构造，因此两者只能
// 先后建立，然后由 Rebind 补齐互相引用。写入回调在 run() 期间才被调用，绑定时序因此安全。
class CancelAfterFramesSink final : public nexweave::capability::IAudioSink {
 public:
  CancelAfterFramesSink(std::size_t cancel_at, FakeAudioSink& observer)
      : cancel_at_(cancel_at), observer_(observer) {}

  // 绑定取消请求的目标。必须在应用开始运行之前调用一次；传 nullptr 关闭取消。
  void Rebind(SessionApp* app) noexcept { app_ = app; }

  OperationResult open() override { return observer_.open(); }

  OperationResult write(const AudioFrame& frame) override {
    const auto written = observer_.write(frame);
    if (!written.ok()) {
      return written;
    }
    ++writes_;
    if (app_ != nullptr && cancel_at_ != 0 && writes_ == cancel_at_) {
      app_->cancel_turn();
    }
    return written;
  }

  OperationResult cancel() noexcept override { return observer_.cancel(); }
  OperationResult close() noexcept override { return observer_.close(); }

 private:
  std::size_t cancel_at_ = 0;
  FakeAudioSink& observer_;
  SessionApp* app_ = nullptr;
  std::size_t writes_ = 0;
};

// 每帧等待的音频源包装：把“采集侧的时间”做成显式的墙钟等待，用于演示“信号能唤醒正在
// 等待输入的进程”。等待只发生在读取下一帧之前，不持锁、不阻塞信号处理，也不改变任何
// 音频语义；pace_us 为 0 时本对象等价于被包装的源。
class PacedAudioSource final : public nexweave::capability::IAudioSource {
 public:
  PacedAudioSource(FakeAudioSource& inner, std::size_t pace_us)
      : inner_(inner), pace_us_(pace_us) {}

  OperationResult open() override { return inner_.open(); }

  nexweave::domain::Result<AudioFrame> read() override {
    if (pace_us_ != 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(pace_us_));
    }
    return inner_.read();
  }

  OperationResult cancel() noexcept override { return inner_.cancel(); }
  OperationResult close() noexcept override { return inner_.close(); }

 private:
  FakeAudioSource& inner_;
  std::size_t pace_us_ = 0;
};

// 识别替补：按轮次把 hypotheses 里的文本作为识别结果交付。它不做任何声音分析，也不声称
// 识别准确率：逐轮文本来自命令行，属于确定性夹具。文本模式的固定文本由应用持有的文本
// 注入器替换结束帧文本，因此本夹具只需要正常响应送帧与结束帧。
class CliAsr final : public nexweave::capability::IAsr {
 public:
  CliAsr(std::vector<std::string> texts, std::string fallback)
      : texts_(std::move(texts)), fallback_(std::move(fallback)) {
    if (texts_.empty()) {
      texts_.push_back(fallback_);
    }
  }

  OperationResult set_callback(TextEventCallback callback) override {
    if (!callback) {
      return OperationResult::failure(ErrorCode::kInvalidInput);
    }
    callback_ = std::move(callback);
    return OperationResult::success();
  }

  OperationResult feed(const AudioFrame& frame, bool is_last) override {
    if (!callback_) {
      return OperationResult::failure(ErrorCode::kInvalidInput);
    }
    ++fed_frames_;
    fed_samples_ += frame.samples.size();
    if (!is_last) {
      // 中间帧交付一条 partial：它证明音频确实是逐帧送进来的，而不是被一次性整段提交。
      callback_(TextEvent{TextEventKind::kPartial, CurrentText(), {}});
      return OperationResult::success();
    }
    const std::string final_text = CurrentText();
    if (cursor_ + 1 < texts_.size()) {
      ++cursor_;
    }
    callback_(TextEvent{TextEventKind::kFinal, final_text, {}});
    return OperationResult::success();
  }

  OperationResult cancel() noexcept override { return OperationResult::success(); }

  std::size_t fed_frames() const noexcept { return fed_frames_; }
  std::size_t fed_samples() const noexcept { return fed_samples_; }

 private:
  // 文本列表耗尽后重复最后一项：长输入不会让夹具越界，也不会伪造新内容。
  const std::string& CurrentText() const noexcept {
    return texts_.at(cursor_ < texts_.size() ? cursor_ : texts_.size() - 1);
  }

  std::vector<std::string> texts_;
  std::string fallback_;
  TextEventCallback callback_;
  std::size_t cursor_ = 0;
  std::size_t fed_frames_ = 0;
  std::size_t fed_samples_ = 0;
};

// 逐轮事件记录：把每一轮的关键事实编码成 observability 事件与指标，供后续的运行证据任务
// 接上清单与时间戳。这里只记录“本轮确实发生了什么”，不推断未测量的量（例如延迟）。
class RunReporter final : public SessionAppObserver {
 public:
  RunReporter(bool print_turns, bool emit_events, bool emit_metrics)
      : print_turns_(print_turns), emit_events_(emit_events), emit_metrics_(emit_metrics) {}

  void on_turn(const SessionTurnResult& result) override {
    ++sequence_;
    const std::string marker =
        result.completed ? "succeeded" : (result.cancelled ? "cancelled" : "failed");
    if (print_turns_) {
      std::cout << "turn " << sequence_ << ": request=" << result.request_id
                << " route=" << RouteName(result.route) << " marker=" << marker
                << " state=" << StateName(result.state)
                << " frames=" << result.pcm_frames.size()
                << " peak_pending=" << result.peak_pending_frames << " text=" << Quote(result.text)
                << std::endl;
      if (!result.error.ok()) {
        std::cout << "  error=" << ErrorName(result.error.code) << " (" << result.error.message
                  << ")" << std::endl;
      }
      if (!result.cleanup_error.ok()) {
        std::cout << "  cleanup_error=" << ErrorName(result.cleanup_error.code) << " ("
                  << result.cleanup_error.message << ")" << std::endl;
      }
    }
    turns_.push_back(TurnSummary(result));
    // 事件与指标是两个独立开关：默认（两个都不给）只打印人可读摘要，因此“什么都不加”
    // 不会往 stdout 混入 JSONL；两个开关都必须被读取，否则命令行契约与 --help 不一致。
    if (emit_events_) {
      if (result.playback_started) {
        EmitEvent(result, "playback_started", "true");
      }
      if (result.interrupted_by_speech) {
        EmitEvent(result, "speech_interrupt", result.interrupt_notice.stream_id);
      }
      EmitEvent(result, "turn_terminal", marker);
    }
    if (emit_metrics_) {
      EmitMetric(result, "playback_peak_pending_frames", "frames",
                 static_cast<double>(result.peak_pending_frames));
      EmitMetric(result, "pcm_frames", "frames", static_cast<double>(result.pcm_frames.size()));
      EmitMetric(result, "answer_bytes", "bytes", static_cast<double>(result.text.size()));
    }
  }

  void on_stream_closed(const std::string& stream_id) override {
    closed_stream_id_ = stream_id;
    ++sequence_;
    nexweave::observability::ObservationEvent event;
    event.request_id = stream_id;
    event.session_id = stream_id;
    event.sequence = sequence_;
    event.name = "stream_closed";
    if (emit_events_) {
      Emit(event);
    }
  }

  const std::vector<nlohmann::json>& turns() const { return turns_; }
  const std::string& closed_stream_id() const { return closed_stream_id_; }

 private:
  static std::string RouteName(RagRouteLevel level) {
    switch (level) {
      case RagRouteLevel::kL0:
        return "L0";
      case RagRouteLevel::kL1:
        return "L1";
      case RagRouteLevel::kL2:
        return "L2";
      case RagRouteLevel::kL3:
        return "L3";
    }
    return "unknown";
  }

  static std::string ErrorName(ErrorCode code) {
    switch (code) {
      case ErrorCode::kNone:
        return "none";
      case ErrorCode::kInvalidInput:
        return "invalid_input";
      case ErrorCode::kMissingField:
        return "missing_field";
      case ErrorCode::kAlreadyCompleted:
        return "already_completed";
      case ErrorCode::kCancelled:
        return "cancelled";
      case ErrorCode::kTimeout:
        return "timeout";
      case ErrorCode::kBackendFailure:
        return "backend_failure";
      case ErrorCode::kDeviceFailure:
        return "device_failure";
    }
    return "unknown";
  }

  static std::string StateName(SessionStateMachine::State state) {
    switch (state) {
      case SessionStateMachine::State::kIdle:
        return "idle";
      case SessionStateMachine::State::kListening:
        return "listening";
      case SessionStateMachine::State::kRouting:
        return "routing";
      case SessionStateMachine::State::kThinking:
        return "thinking";
      case SessionStateMachine::State::kSpeaking:
        return "speaking";
      case SessionStateMachine::State::kCancelling:
        return "cancelling";
    }
    return "unknown";
  }

  // 摘要里的文本按单行输出：多行回答在终端与 JSONL 里都不便于比对，因此换行统一转义。
  static std::string Quote(const std::string& text) {
    std::string output;
    output.reserve(text.size() + 2);
    output.push_back('"');
    for (const char character : text) {
      if (character == '\n') {
        output += "\\n";
      } else if (character == '"') {
        output += "\\\"";
      } else {
        output.push_back(character);
      }
    }
    output.push_back('"');
    return output;
  }

  static nlohmann::json TurnSummary(const SessionTurnResult& result) {
    nlohmann::json summary;
    summary["request_id"] = result.request_id;
    summary["route"] = RouteName(result.route);
    summary["state"] = StateName(result.state);
    summary["marker"] = result.completed ? "succeeded"
                                         : (result.cancelled ? "cancelled" : "failed");
    summary["completed"] = result.completed;
    summary["cancelled"] = result.cancelled;
    summary["playback_started"] = result.playback_started;
    summary["playback_done"] = result.playback_done;
    summary["backpressure"] = result.backpressure;
    summary["interrupted_by_speech"] = result.interrupted_by_speech;
    summary["text"] = result.text;
    summary["pcm_frames"] = result.pcm_frames.size();
    summary["peak_pending_frames"] = result.peak_pending_frames;
    summary["error"] = ErrorName(result.error.code);
    summary["error_message"] = result.error.message;
    summary["cleanup_error"] = ErrorName(result.cleanup_error.code);
    return summary;
  }

  // 事件与指标都归到同一个会话上：流关闭之后用流标识，使收尾事件仍可归属；关闭之前用
  // 请求标识。回退规则只写一次，避免两类记录各自维护一份而将来出现分歧。
  std::string session_of(const SessionTurnResult& result) const {
    return closed_stream_id_.empty() ? result.request_id : closed_stream_id_;
  }

  void EmitEvent(const SessionTurnResult& result, const std::string& name,
                 const std::string& attribute) {
    nexweave::observability::ObservationEvent event;
    event.request_id = result.request_id;
    event.session_id = session_of(result);
    event.sequence = sequence_;
    event.name = name;
    event.attributes["value"] = attribute;
    Emit(event);
  }

  void EmitMetric(const SessionTurnResult& result, const std::string& name,
                  const std::string& unit, double value) {
    nexweave::observability::MetricRecord metric;
    metric.request_id = result.request_id;
    metric.session_id = session_of(result);
    metric.name = name;
    metric.unit = unit;
    metric.value = value;
    const auto encoded = nexweave::observability::encode_metric(metric);
    if (encoded.ok()) {
      std::cout << *encoded.value << std::endl;
    }
  }

  void Emit(const nexweave::observability::ObservationEvent& event) {
    const auto encoded = nexweave::observability::encode_event(event);
    if (encoded.ok()) {
      std::cout << *encoded.value << std::endl;
    }
  }

  bool print_turns_ = true;
  bool emit_events_ = false;
  bool emit_metrics_ = false;
  std::size_t sequence_ = 0;
  std::vector<nlohmann::json> turns_;
  std::string closed_stream_id_;
};

// 确定性活动脚本：开头静音 + 若干次（人声 + 双倍静音超时）。静音段长度取静音超时的
// 倍数，因此每次说话都会在确定位置交付一个段，不需要真实时间也不需要睡眠。
std::vector<ActivityRun> build_resident_script(const SessionAppConfig& config,
                                             std::size_t utterances,
                                             std::size_t lead_in_silence,
                                             std::size_t speech_frames) {
  const auto& segmentation = config.resident_config.segmentation;
  std::vector<ActivityRun> runs;
  if (lead_in_silence > 0) {
    runs.push_back(ActivityRun{lead_in_silence, SpeechActivity::kSilence});
  }
  for (std::size_t index = 0; index < utterances; ++index) {
    runs.push_back(ActivityRun{speech_frames, SpeechActivity::kSpeech});
    runs.push_back(ActivityRun{2 * segmentation.min_silence_frames, SpeechActivity::kSilence});
  }
  return runs;
}

// 把活动脚本展开成逐帧样本：人声与静音取不同电平，使“送进识别的是哪一段”可以用样本内容
// 核对，而不是只数帧数。
std::vector<std::int16_t> build_resident_samples(const std::vector<ActivityRun>& runs) {
  std::vector<std::int16_t> samples;
  for (const auto& run : runs) {
    const std::int16_t value = run.activity == SpeechActivity::kSpeech ? 6000 : 120;
    samples.insert(samples.end(), run.frames * kFrameSamples, value);
  }
  return samples;
}

// 内建的路由夹具。两条记录覆盖三种路由级别：命中高分记录是 L1 直答，命中低分记录是 L2
// 携带上下文生成，没有命中是 L3 普通对话。分数是夹具排序量，不是概率；真实知识库与
// 路由校准由后续任务交付。
std::vector<RetrievedChunk> builtin_knowledge() {
  return {
      RetrievedChunk{"geo-capital-fr", "the capital of france is paris", 0.97},
      RetrievedChunk{"weather-paris", "the weather in paris is sunny and dry today", 0.60},
  };
}

void print_usage() {
  std::cout
      << "用法: nexweave_session_app --input <text|file|resident> [选项]\n"
         "\n"
         "输入模式（互斥，必须选一个）：\n"
         "  --input text            固定文本输入：--text <文本> 就是这一轮的识别结果\n"
         "                              （文件与常驻模式不使用 --text，识别文本来自 --hypotheses）\n"
         "  --input file            固定文件音频：--wav <路径>，只接受 16 kHz/单声道/16 位 PCM\n"
         "  --input resident        模拟常驻输入：按活动脚本生成多轮语音段\n"
         "\n"
         "通用选项：\n"
         "  --stream-id <id>        会话与输入流标识（默认 session-app）\n"
         "  --max-turns <n>         最多执行的轮次，0 表示不限（默认 0）\n"
         "  --playback-capacity <n> 播放缓冲上限（帧），达到即本轮背压失败（默认 256）\n"
         "  --text-chunk-bytes <n>  生成文本的片段字节上限（默认 60，必须大于 0）\n"
         "  --events                在 stdout 输出逐轮事件 JSONL\n"
         "  --metrics               在 stdout 输出逐轮指标 JSONL\n"
         "  --summary-json          在 stdout 输出机器可读的汇总 JSON\n"
         "  --quiet                 不打印逐轮摘要，只输出事件/指标/汇总\n"
         "  --help                  打印本说明\n"
         "\n"
         "常驻模式选项：\n"
         "  --utterances <n>            活动脚本中的说话次数（默认 2）\n"
         "  --speech-frames <n>         每次说话的人声帧数（默认 40）\n"
         "  --lead-in-silence <n>       开头静音帧数（默认 10）\n"
         "  --preroll-frames <n>        段头前置缓冲帧数（默认 2）\n"
         "  --min-silence-frames <n>    判定说话结束的连续静音帧数（默认 8）\n"
         "  --min-speech-frames <n>     交付一段所需的最少人声帧数（默认 4）\n"
         "  --max-speech-frames <n>     单块人声帧数上限（默认 200）\n"
         "  --pending-segments <n>      已完成但未取走的段上限（默认 4）\n"
         "  --pump-budget <n>           一次取段最多泵入的帧数（默认 64，必须大于 0）\n"
         "  --pace-us <n>               每帧读取前的墙钟等待（微秒），用于演示信号唤醒等待\n"
         "  --hypotheses <a,停止>       逐轮识别文本，逗号分隔（默认 知识问句,停止指令）；\n"
         "                              文本模式请用 --text 指定唯一一轮的文本\n"
         "  --cancel-after-frames <n>   第 n 帧播出后取消本轮，0 表示不取消（默认 0）\n"
         "\n"
         "退出码：正常收敛为 0（含被取消、被停止、空输入）；配置错误或运行失败为 1。\n";
}

// 逗号分隔的文本列表；空项被忽略，因此末尾多写一个逗号也不会产生空文本。
std::vector<std::string> split_list(const std::string& text) {
  std::vector<std::string> items;
  std::string current;
  for (const char character : text) {
    if (character == ',') {
      if (!current.empty()) {
        items.push_back(current);
      }
      current.clear();
    } else {
      current.push_back(character);
    }
  }
  if (!current.empty()) {
    items.push_back(current);
  }
  return items;
}

bool parse_size(const std::string& text, std::size_t& value) {
  try {
    const auto parsed = static_cast<long long>(std::stoll(text));
    if (parsed < 0) {
      return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

// 命令行到应用配置的映射。每个字段都能在 --help 里找到对应说明。
struct Options {
  std::string input;
  std::string stream_id = "session-app";
  std::string text;
  std::string wav_path;
  std::string hypotheses;
  std::size_t max_turns = 0;
  std::size_t playback_capacity = 256;
  std::size_t text_chunk_bytes = 60;
  std::size_t utterances = 2;
  std::size_t speech_frames = 40;
  std::size_t lead_in_silence = 10;
  std::size_t preroll_frames = 2;
  std::size_t min_silence_frames = 8;
  std::size_t min_speech_frames = 4;
  std::size_t max_speech_frames = 200;
  std::size_t pending_segments = 4;
  std::size_t pump_budget = 64;
  std::size_t pace_us = 0;
  std::size_t cancel_after_frames = 0;
  bool emit_events = false;
  bool emit_metrics = false;
  bool summary_json = false;
  bool quiet = false;
};

// 参数解析的三态结果。用枚举而不是“布尔返回值 + 布尔出参”：两个布尔会出现四种组合，
// 其中一种没有意义（打印帮助又算参数错误），调用方也就不得不写一个永远走不到的分支。
enum class ParseOutcome {
  kContinue,
  kHelpPrinted,
  kInvalid,
};

ParseOutcome parse_options(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    const auto value = [index, argc, argv]() -> std::string {
      return index + 1 < argc ? std::string(argv[index + 1]) : std::string();
    };
    const auto take = [&index]() { ++index; };
    // 取值类开关共用同一种解析：失败时打印带开关名的原因，成功时消费掉取值。
    bool size_ok = true;
    const auto size_option = [&](std::size_t& target) {
      if (!parse_size(value(), target)) {
        std::cerr << flag << " 需要非负整数" << std::endl;
        size_ok = false;
        return;
      }
      take();
    };
    const auto text_option = [&](std::string& target) {
      target = value();
      take();
    };

    if (flag == "--help" || flag == "-h") {
      print_usage();
      return ParseOutcome::kHelpPrinted;
    } else if (flag == "--events") {
      options.emit_events = true;
    } else if (flag == "--metrics") {
      options.emit_metrics = true;
    } else if (flag == "--summary-json") {
      options.summary_json = true;
    } else if (flag == "--quiet") {
      options.quiet = true;
    } else if (flag == "--input") {
      text_option(options.input);
    } else if (flag == "--stream-id") {
      text_option(options.stream_id);
    } else if (flag == "--text") {
      text_option(options.text);
    } else if (flag == "--wav") {
      text_option(options.wav_path);
    } else if (flag == "--hypotheses") {
      text_option(options.hypotheses);
    } else if (flag == "--max-turns") {
      size_option(options.max_turns);
    } else if (flag == "--playback-capacity") {
      size_option(options.playback_capacity);
    } else if (flag == "--text-chunk-bytes") {
      size_option(options.text_chunk_bytes);
    } else if (flag == "--utterances") {
      size_option(options.utterances);
    } else if (flag == "--speech-frames") {
      size_option(options.speech_frames);
    } else if (flag == "--lead-in-silence") {
      size_option(options.lead_in_silence);
    } else if (flag == "--preroll-frames") {
      size_option(options.preroll_frames);
    } else if (flag == "--min-silence-frames") {
      size_option(options.min_silence_frames);
    } else if (flag == "--min-speech-frames") {
      size_option(options.min_speech_frames);
    } else if (flag == "--max-speech-frames") {
      size_option(options.max_speech_frames);
    } else if (flag == "--pending-segments") {
      size_option(options.pending_segments);
    } else if (flag == "--pump-budget") {
      size_option(options.pump_budget);
    } else if (flag == "--pace-us") {
      size_option(options.pace_us);
    } else if (flag == "--cancel-after-frames") {
      size_option(options.cancel_after_frames);
    } else {
      std::cerr << "未知参数: " << flag << "（用 --help 查看用法）" << std::endl;
      return ParseOutcome::kInvalid;
    }
    if (!size_ok) {
      return ParseOutcome::kInvalid;
    }
  }
  return ParseOutcome::kContinue;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  switch (parse_options(argc, argv, options)) {
    case ParseOutcome::kHelpPrinted:
      return 0;  // 打印帮助是正常结束，不是参数错误。
    case ParseOutcome::kInvalid:
      return 1;
    case ParseOutcome::kContinue:
      break;
  }
  if (options.input != "text" && options.input != "file" && options.input != "resident") {
    std::cerr << "--input 必须是 text、file 或 resident" << std::endl;
    return 1;
  }

  SessionAppConfig config;
  config.stream_id = options.stream_id;
  config.max_turns = options.max_turns;
  config.session_config.playback_queue_capacity = options.playback_capacity;
  config.session_config.text_chunk_max_bytes = options.text_chunk_bytes;
  config.pump_budget_per_take = options.pump_budget;
  config.resident_config.pending_segment_capacity = options.pending_segments;
  config.resident_config.segmentation.preroll_frames = options.preroll_frames;
  config.resident_config.segmentation.min_silence_frames = options.min_silence_frames;
  config.resident_config.segmentation.min_speech_frames = options.min_speech_frames;
  config.resident_config.segmentation.max_speech_frames = options.max_speech_frames;

  // 能力夹具全部是确定性的内存实现：只有 --wav 会读文件，其余不访问声卡、网络或模型运行时。
  FakeRag retriever(builtin_knowledge());
  FakeRagRouter router(retriever);
  FakeTts tts;
  const std::vector<std::string> tokens = {"第一句回答。", "第二句回答。", "第三句回答。"};
  FakeLlm llm(tokens);
  SteadyPlaybackClock clock;

  // 常驻模式的夹具：音频源与判定器必须比常驻输入活得久，因此与输入一起声明在这一层，
  // 生命周期覆盖整个 main。
  std::vector<ActivityRun> resident_runs;
  std::vector<std::int16_t> resident_samples;
  std::unique_ptr<FakeAudioSource> resident_source;
  std::unique_ptr<PacedAudioSource> resident_paced;
  std::unique_ptr<SpeechActivityScript> resident_detector;
  std::unique_ptr<ResidentAudioInput> resident;

  std::vector<std::string> hypotheses;
  if (!options.hypotheses.empty()) {
    hypotheses = split_list(options.hypotheses);
  }

  if (options.input == "text") {
    config.mode = SessionAppInputMode::kText;
    config.text = options.text;
  } else if (options.input == "file") {
    config.mode = SessionAppInputMode::kWavFile;
    config.wav_path = options.wav_path;
  } else {
    config.mode = SessionAppInputMode::kSimulatedResident;
    resident_runs = build_resident_script(config, options.utterances, options.lead_in_silence,
                                        options.speech_frames);
    resident_samples = build_resident_samples(resident_runs);
    resident_source.reset(new FakeAudioSource(resident_samples));
    resident_paced.reset(new PacedAudioSource(*resident_source, options.pace_us));
    resident_detector.reset(new SpeechActivityScript(resident_runs));
    resident.reset(new ResidentAudioInput(*resident_paced, *resident_detector, options.stream_id,
                                          config.resident_config));
    if (hypotheses.empty()) {
      hypotheses.push_back("the capital of france");
      hypotheses.push_back("停止");
    }
  }

  // 音频汇由本函数拥有：播放组件只负责“把帧写出去”，打开与关闭设备是拥有者的责任，
  // 因此这里必须先 open()，否则每一次写入都会以设备未打开告终。
  FakeAudioSink sink;
  const auto sink_opened = sink.open();
  if (!sink_opened.ok()) {
    std::cerr << "音频汇打开失败: " << sink_opened.error.message << std::endl;
    return 1;
  }
  // 播放组件：默认直接把帧写进内存音频汇。启用“第 N 帧后取消”时改由取消夹具包住同一个
  // 汇，使取消发生在“帧已经写出”之后；两种情况下应用看到的都是同一个 IAudioPlayback 契约。
  std::unique_ptr<CancelAfterFramesSink> cancel_sink;
  if (options.cancel_after_frames != 0) {
    cancel_sink.reset(new CancelAfterFramesSink(options.cancel_after_frames, sink));
  }
  nexweave::capability::IAudioSink& active_sink =
      cancel_sink != nullptr ? static_cast<nexweave::capability::IAudioSink&>(*cancel_sink) : sink;
  LogicalClockPlayback active_playback(active_sink, clock, static_cast<std::int64_t>(kFrameMs));
  CliAsr asr(hypotheses, options.text);

  SessionApp app(config, asr, retriever, router, tts, active_playback,
                 resident ? resident.get() : nullptr, &llm);
  if (cancel_sink != nullptr) {
    // 互相引用的补齐点：应用已经建立，取消夹具现在可以回调它请求取消。
    cancel_sink->Rebind(&app);
  }
  RunReporter reporter(!options.quiet, options.emit_events, options.emit_metrics);

  // 只注册本进程真正拥有生命周期的两个信号：其余信号保持默认行为，退出码因此仍然能
  // 区分“按契约收敛”与“被外部强杀”。
  g_app = &app;
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  std::cout << "session_app: mode=" << options.input << " stream=" << options.stream_id
            << " max_turns=" << options.max_turns << std::endl;

  const auto run = app.run(&reporter);

  const bool interrupted = g_signal != 0;
  if (!run.error.ok()) {
    std::cerr << "运行失败: " << run.error.message << std::endl;
  }
  if (!run.cleanup_error.ok()) {
    std::cerr << "输入清理失败: " << run.cleanup_error.message << std::endl;
  }
  if (!options.quiet && options.input == "resident") {
    std::cout << "resident: frames_pumped=" << run.frames_pumped
              << " segments_queued=" << run.segments_queued
              << " segments_dropped=" << run.segments_dropped
              << " signal=" << (interrupted ? "yes" : "no") << std::endl;
  }

  if (options.summary_json) {
    nlohmann::json summary;
    summary["mode"] = options.input;
    summary["stream_id"] = options.stream_id;
    summary["turns_started"] = run.turns.size();
    summary["turns_completed"] = run.turns_completed;
    summary["turns_failed"] = run.turns_failed;
    summary["input_ended"] = run.input_ended;
    summary["stopped_early"] = run.stopped_early;
    summary["stream_closed"] = !reporter.closed_stream_id().empty();
    summary["error"] = run.error.ok() ? "none" : "failed";
    summary["cleanup_error"] = run.cleanup_error.ok() ? "none" : "failed";
    summary["signal"] = interrupted;
    summary["frames_pumped"] = run.frames_pumped;
    summary["segments_queued"] = run.segments_queued;
    summary["segments_dropped"] = run.segments_dropped;
    summary["asr_fed_frames"] = asr.fed_frames();
    summary["sink_writes"] = sink.frames().size();
    summary["turns"] = reporter.turns();
    std::cout << summary.dump() << std::endl;
  }

  return run.error.ok() ? 0 : 1;
}

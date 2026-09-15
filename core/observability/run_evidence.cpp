// 运行证据记录器的实现。职责、时间族划分与并发约定见头文件；本文件只说明“为什么这样组织”。
//
// 为什么用一张里程碑表承载全部事实
// --------------------------------
// 事件、指标与摘要都是同一批里程碑的不同投影：事件是逐条原文，指标是它们的时间与步数，
// 摘要是给人读的表格。若三者各自记录一次，任何一次新增或改名都要改三处，而漏改的那一处
// 不会报错、只会静默地少一条证据。因此这里只有一个真相来源（milestones_），其余全是投影。
//
// 为什么“首次出现”而不是“最后一次”
// ---------------------------------
// 常驻会话可以连续多轮，同一个里程碑（例如旧输出封锁）会出现多次。指标回答的是“这件事
// 第一次发生在什么时候”，用它去算首 token 延迟、重叠领先量才有意义；取最后一次会让多轮
// 会话的指标随轮数漂移，而它与“首次”早已不是同一个问题。事件流保留全部出现，不丢事实。
#include "run_evidence.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>

namespace nexweave::observability {
namespace {

// 取消阶段的固定顺序。摘要在报告取消时按它列出阶段，因此“阶段齐全”与“顺序正确”两件事
// 在人的眼里和机器眼里是同一份定义。
const char* const kCancelPhases[] = {
    milestone::kCancelAccepted,  milestone::kOldOutputBlocked, milestone::kExecutionExited,
    milestone::kPlaybackCleared, milestone::kTerminalCancelled,
};

// 需要两个里程碑相减才能得到的单调指标：起点与终点写在表里，指标名只是它们的标签。
// 集中成一张表而不是散落在渲染分支里，是为了让“这个数从哪两个时刻算出来”永远和指标名
// 写在同一行。
struct DerivedMetric {
  const char* name;
  const char* unit;
  const char* start;
  const char* end;
};

const DerivedMetric kDerivedMonotonic[] = {
    {"mono_overlap_lead_us", "us", milestone::kPlaybackStart, milestone::kGenerationDone},
    {"mono_cancel_cleared_us", "us", milestone::kCancelAccepted, milestone::kPlaybackCleared},
    {"mono_cancel_terminal_us", "us", milestone::kCancelAccepted, milestone::kTerminalCancelled},
};

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 日历时间文本（UTC，ISO-8601，秒精度）。它只用于把一次执行关联到真实时刻，因此不需要
// 亚秒精度，也不需要时区。
std::string Iso8601UtcNow() {
  const std::time_t seconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm parts{};
#if defined(_WIN32)
  if (gmtime_s(&parts, &seconds) != 0) {
    return std::string("none");
  }
#else
  if (gmtime_r(&seconds, &parts) == nullptr) {
    return std::string("none");
  }
#endif
  char buffer[32];
  const std::size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts);
  return written == 0 ? std::string("none") : std::string(buffer);
}

// 指标的“起点 → 终点”口径说明。它只写进人类可读的摘要：metrics.jsonl 用
// name/unit/value 三个字段表达事实，而口径是同一件事的解释，落在需要解释的地方。
std::string MetricScope(std::string_view name) {
  for (const DerivedMetric& derived : kDerivedMonotonic) {
    if (name == derived.name) {
      std::string text(derived.start);
      text += " → ";
      text += derived.end;
      if (name == "mono_overlap_lead_us") {
        text += "；为正表示生成结束前已经开始播放（重叠），为负表示文本先定稿再开始播放"
                "（L0/L1 串行直答）";
      }
      return text;
    }
  }
  if (name == "mono_run_total_us") {
    return "run_start → run_end";
  }
  if (name == "step_total") {
    return "本次运行记录的里程碑总数（确定性，可复现）";
  }
  if (name == "step_audio_frames") {
    return "本次运行写出音频汇的帧数，取自会话运行记录";
  }
  if (name == "step_audio_duration_ms") {
    return "帧数 × 帧长，按音频契约推导的设计值，不是实测播放时长";
  }
  if (starts_with(name, "mono_") && ends_with(name, "_us")) {
    const std::string middle(name.substr(5, name.size() - 5 - 3));
    return "run_start → " + middle + "，本机本负载下的单调实测值";
  }
  if (starts_with(name, "step_")) {
    const std::string middle(name.substr(5));
    return middle + " 的调度步数（确定性，可复现）";
  }
  return "未定义";
}

// Markdown 表格里的竖线与换行会破坏表格；证据里的文本多来自受控常量，但环境字段可能来自
// 外部（编译器版本、Git 提交）。转义而不是原样拼接，避免一条含竖线的诊断把整张表错位。
std::string EscapeCell(const std::string& text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char character : text) {
    if (character == '|' || character == '\n' || character == '\r') {
      escaped.push_back(' ');
    } else {
      escaped.push_back(character);
    }
  }
  return escaped;
}

// 反引号包裹的行内代码。空串写成 `none`，因为 Markdown 里的空反引号会渲染成空单元格，
// 让“这个字段没有值”和“这一行没渲染出来”看起来一样。
std::string Code(const std::string& text) {
  return "`" + EscapeCell(text.empty() ? std::string("none") : text) + "`";
}

}  // namespace

const char* const kAllMilestones[] = {
    milestone::kRunStart,          milestone::kGenerationStarted,
    milestone::kFirstToken,        milestone::kFirstPcm,
    milestone::kPlaybackStart,     milestone::kGenerationDone,
    milestone::kSynthesisDone,     milestone::kPlaybackDone,
    milestone::kCancelAccepted,    milestone::kOldOutputBlocked,
    milestone::kExecutionExited,   milestone::kPlaybackCleared,
    milestone::kTerminalSucceeded, milestone::kTerminalCancelled,
    milestone::kRunEnd,
};
const std::size_t kAllMilestoneCount = sizeof(kAllMilestones) / sizeof(kAllMilestones[0]);

const char* marker_milestone_name(runtime::ActivityMarker marker) noexcept {
  switch (marker) {
    case runtime::ActivityMarker::kGenerationStarted:
      return milestone::kGenerationStarted;
    case runtime::ActivityMarker::kPlaybackStarted:
      // 会话侧的“第一帧已经交给播放组件”。设备侧的“第一帧已经写出去”由音频汇单独报告，
      // 两者是不同所有者的边界，因此是两条里程碑。
      return milestone::kFirstPcm;
    case runtime::ActivityMarker::kGenerationDone:
      return milestone::kGenerationDone;
    case runtime::ActivityMarker::kSynthesisDone:
      return milestone::kSynthesisDone;
    case runtime::ActivityMarker::kPlaybackDone:
      return milestone::kPlaybackDone;
    case runtime::ActivityMarker::kCancelAccepted:
      return milestone::kCancelAccepted;
    case runtime::ActivityMarker::kOldOutputBlocked:
      return milestone::kOldOutputBlocked;
    case runtime::ActivityMarker::kExecutionExited:
      return milestone::kExecutionExited;
    case runtime::ActivityMarker::kPlaybackCleared:
      return milestone::kPlaybackCleared;
    case runtime::ActivityMarker::kTerminalSucceeded:
      return milestone::kTerminalSucceeded;
    case runtime::ActivityMarker::kTerminalCancelled:
      return milestone::kTerminalCancelled;
  }
  return "";
}

std::int64_t SteadyMonotonicClock::now_us() const noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

RunEvidenceRecorder::RunEvidenceRecorder(const RunEvidenceConfig& config,
                                         const IMonotonicClock& clock)
    : config_(config), clock_(clock), origin_us_(clock.now_us()) {
  start_time_ = Iso8601UtcNow();
  // run_start 是构造事实而不是调用方打点：任何需要记录的时刻都晚于它，因此它的存在
  // 不需要调用方记得多做一次调用。
  append_locked(milestone::kRunStart, 0, "runner", 0);
}

void RunEvidenceRecorder::append_locked(const char* name, std::uint64_t generation,
                                        const char* source, std::int64_t mono_us) {
  MilestoneRecord record;
  record.name = name;
  record.request_id = config_.request_id;
  record.session_id = config_.session_id;
  record.generation = generation;
  record.step = next_step_++;
  record.mono_us = mono_us;
  record.source = source;
  milestones_.push_back(std::move(record));
}

bool RunEvidenceRecorder::first_index_locked(std::string_view name, std::size_t& index) const {
  for (std::size_t position = 0; position < milestones_.size(); ++position) {
    if (milestones_[position].name == name) {
      index = position;
      return true;
    }
  }
  return false;
}

std::string RunEvidenceRecorder::unmeasured_reason_locked(const char* name) const {
  const std::string_view milestone_name(name);
  std::size_t index = 0;
  if (first_index_locked(milestone_name, index)) {
    return std::string();
  }
  // 缺失的原因必须具体：把“路径不同”和“阶段没走到”混成一句“未测量”，读者就无法判断
  // 这是正常形态还是实现漏报。
  if (milestone_name == milestone::kFirstToken) {
    if (!generation_failure_.empty()) {
      return "生成后端报告失败：" + EscapeCell(generation_failure_);
    }
    return "本次运行没有从生成后端收到 token（L0/L1 直答路径，或后端未实现进度接缝）";
  }
  if (milestone_name == milestone::kTerminalSucceeded) {
    return "本次运行以取消或失败收敛，没有成功终态";
  }
  if (milestone_name == milestone::kCancelAccepted ||
      milestone_name == milestone::kOldOutputBlocked ||
      milestone_name == milestone::kExecutionExited ||
      milestone_name == milestone::kPlaybackCleared ||
      milestone_name == milestone::kTerminalCancelled) {
    return "本次运行没有发生取消";
  }
  if (milestone_name == milestone::kRunStart || milestone_name == milestone::kRunEnd) {
    return "记录器尚未确定运行边界（不应出现）";
  }
  if (finished_ && outcome_.error_code != "none") {
    return "本次运行以错误收敛（" + EscapeCell(outcome_.error_code) + "），未到达该阶段";
  }
  return "本次运行未走到该阶段（被取消，或输入在开始任何轮次之前就不可用）";
}

void RunEvidenceRecorder::on_marker(runtime::ActivityMarker marker, std::uint64_t generation) {
  const char* const name = marker_milestone_name(marker);
  if (name[0] == '\0') {
    // 未覆盖的枚举值既不能猜一个名字，也不能静默丢弃：静默丢弃会让新增标记在证据里
    // 凭空消失，而那正是本层要避免的。这里如实跳过，并由候选全集的“未测量”列暴露出来。
    return;
  }
  const std::lock_guard<std::mutex> guard(mutex_);
  if (finished_) {
    // 冻结之后的迟到回调（例如清理期间才送达的终态）不再入表：摘要与指标必须描述同一份
    // 快照，多一条事件就会让“谁在 finish 之后写的”变成一个无法回答的问题。
    return;
  }
  append_locked(name, generation, "session", clock_.now_us() - origin_us_);
}

void RunEvidenceRecorder::on_token_delivered(const std::string& /*token*/) {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (finished_ || first_token_recorded_) {
    return;
  }
  first_token_recorded_ = true;
  append_locked(milestone::kFirstToken, 0, "generation", clock_.now_us() - origin_us_);
}

void RunEvidenceRecorder::on_generation_started() {}

void RunEvidenceRecorder::on_generation_completed() {}

void RunEvidenceRecorder::on_generation_failed(const std::string& message) {
  const std::lock_guard<std::mutex> guard(mutex_);
  // 只留首个原因，与项目其它收尾路径一致：后续失败通常只是它的后果，覆盖它会丢掉根因。
  if (generation_failure_.empty()) {
    generation_failure_ = message;
  }
}

void RunEvidenceRecorder::on_first_frame_written() {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (finished_ || first_frame_recorded_) {
    return;
  }
  first_frame_recorded_ = true;
  append_locked(milestone::kPlaybackStart, 0, "device", clock_.now_us() - origin_us_);
}

void RunEvidenceRecorder::finish(const RunOutcome& outcome) {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (finished_) {
    return;
  }
  // 先取结束时刻，再写 run_end：这样 run_end 的单调时间与 end_us_ 是同一个读数，
  // 不会因为两次读数之间的微小间隔让摘要与指标出现两个不同的“运行结束”。
  end_us_ = clock_.now_us() - origin_us_;
  outcome_ = outcome;
  append_locked(milestone::kRunEnd, 0, "runner", end_us_);
  end_time_ = Iso8601UtcNow();
  finished_ = true;
}

bool RunEvidenceRecorder::finished() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return finished_;
}

std::vector<MilestoneRecord> RunEvidenceRecorder::milestones() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return milestones_;
}

bool RunEvidenceRecorder::has_milestone(std::string_view name) {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::size_t index = 0;
  return first_index_locked(name, index);
}

std::uint64_t RunEvidenceRecorder::step_of(std::string_view name, bool& found) {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::size_t index = 0;
  if (!first_index_locked(name, index)) {
    found = false;
    return 0;
  }
  found = true;
  return milestones_[index].step;
}

std::int64_t RunEvidenceRecorder::mono_us_of(std::string_view name, bool& found) {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::size_t index = 0;
  if (!first_index_locked(name, index)) {
    found = false;
    return 0;
  }
  found = true;
  return milestones_[index].mono_us;
}

bool RunEvidenceRecorder::overlap_proven() {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::size_t playback = 0;
  std::size_t generation = 0;
  if (!first_index_locked(milestone::kPlaybackStart, playback) ||
      !first_index_locked(milestone::kGenerationDone, generation)) {
    return false;
  }
  // 用步数而不是时间比较：确定性夹具整轮不到一毫秒，毫秒级时间会把两个阶段压成同一时刻，
  // 而步序不会。这也让“重叠已证明”这条结论与主机无关、可复现。
  return playback < generation;
}

std::size_t RunEvidenceRecorder::milestone_count() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return milestones_.size();
}

bool RunEvidenceRecorder::cancellation_observed() {
  return has_milestone(milestone::kCancelAccepted);
}

bool RunEvidenceRecorder::generation_tokens_observed() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return first_token_recorded_;
}

std::string RunEvidenceRecorder::manifest_json() {
  std::string start_time;
  std::string end_time;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    start_time = start_time_;
    end_time = end_time_;
  }
  RunManifest manifest;
  manifest.run_id = config_.run_id;
  manifest.git_commit = config_.environment.git_commit;
  manifest.compiler = config_.environment.compiler;
  manifest.cmake = config_.environment.cmake;
  manifest.runtime = config_.environment.runtime;
  manifest.driver = config_.environment.driver;
  manifest.model = config_.environment.model;
  manifest.config_hash = config_.config_hash;
  manifest.input_hash = config_.input_hash;
  manifest.device = config_.environment.device;
  manifest.profile = config_.profile;
  manifest.command = config_.command;
  // 日历时间在收尾时确定；尚未收尾时写 none，使“清单里的时间是空的”不会被误读成
  // “这次运行发生在 1970 年”。清单校验只要求非空，因此 none 是合法且诚实的占位。
  manifest.start_time = start_time.empty() ? std::string("none") : start_time;
  manifest.end_time = end_time.empty() ? std::string("none") : end_time;
  const auto encoded = encode_manifest(manifest);
  return encoded.ok() ? *encoded.value : std::string();
}

std::string RunEvidenceRecorder::events_jsonl() {
  std::vector<MilestoneRecord> records;
  std::size_t audio_frames = 0;
  RunOutcome outcome;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    records = milestones_;
    audio_frames = outcome_.audio_frames;
    outcome = outcome_;
  }
  // config_ 在构造之后不再变化，因此下面这些读取不需要持锁。
  const std::string frame_ms = std::to_string(config_.frame_ms);
  std::string lines;
  for (const MilestoneRecord& record : records) {
    ObservationEvent event;
    event.request_id = record.request_id;
    event.session_id = record.session_id;
    event.generation = record.generation;
    // sequence 用调度步数：它在一个 request/session/generation 内单调，而且与主机无关。
    event.sequence = record.step;
    // 契约字段是毫秒（见 ObservationEvent 的说明）；同一个时刻的微秒读数另放在属性里，
    // 因为毫秒分辨率不足以区分确定性夹具里相邻的阶段，而契约字段不允许改成微秒。
    event.timestamp_ms = static_cast<std::uint64_t>(record.mono_us < 0 ? 0 : record.mono_us / 1000);
    event.name = record.name;
    event.attributes["step"] = std::to_string(record.step);
    event.attributes["mono_us"] = std::to_string(record.mono_us);
    event.attributes["source"] = record.source;
    if (record.name == milestone::kRunStart) {
      // 命令哈希与两个已有哈希并列放在起点事件上：清单的字段集合是 v1 契约，加字段需要改
      // 协议版本，而“这次跑的是哪条命令”属于运行身份，放在起点事件上与清单的 command
      // 互为对照。
      event.attributes["command_hash"] = config_.command_hash;
      event.attributes["config_hash"] = config_.config_hash;
      event.attributes["input_hash"] = config_.input_hash;
      event.attributes["frame_ms"] = frame_ms;
    }
    if (record.name == milestone::kRunEnd) {
      event.attributes["exit_code"] = std::to_string(outcome.exit_code);
      event.attributes["error_code"] = outcome.error_code;
      event.attributes["expectation_matched"] = outcome.expectation_matched ? "true" : "false";
      event.attributes["audio_frames"] = std::to_string(audio_frames);
    }
    const auto encoded = encode_event(event);
    if (!encoded.ok()) {
      // 一条编码失败就放弃整份产物：留下前面若干行会让读者以为证据是完整的。
      return std::string();
    }
    lines += *encoded.value;
    lines.push_back('\n');
  }
  return lines;
}

std::string RunEvidenceRecorder::metrics_jsonl() {
  std::vector<MilestoneRecord> records;
  bool frozen = false;
  std::int64_t total_us = 0;
  std::size_t audio_frames = 0;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    records = milestones_;
    frozen = finished_;
    total_us = end_us_;
    audio_frames = outcome_.audio_frames;
  }
  const std::int64_t frame_ms = config_.frame_ms;
  const auto first = [&records](std::string_view name, std::size_t& index) {
    for (std::size_t position = 0; position < records.size(); ++position) {
      if (records[position].name == name) {
        index = position;
        return true;
      }
    }
    return false;
  };
  const auto emit = [](std::string& out, const MilestoneRecord& owner, const std::string& name,
                       const std::string& unit, double value) {
    MetricRecord metric;
    metric.request_id = owner.request_id;
    metric.session_id = owner.session_id;
    metric.generation = owner.generation;
    metric.timestamp_ms =
        static_cast<std::uint64_t>(owner.mono_us < 0 ? 0 : owner.mono_us / 1000);
    metric.name = name;
    metric.unit = unit;
    metric.value = value;
    const auto encoded = encode_metric(metric);
    if (!encoded.ok()) {
      return false;
    }
    out += *encoded.value;
    out.push_back('\n');
    return true;
  };

  std::string lines;
  // 每个**首次出现**的里程碑产生一对指标：单调时间（实测）与调度步数（确定性）。逐条
  // 对齐地产生，读者不需要在两张表之间做映射。同名里程碑在多轮会话里会重复出现，这里
  // 只取第一次：指标回答的是"这件事第一次发生在什么时候"，逐次发生的事实由事件流保留。
  std::vector<std::string> emitted;
  for (const MilestoneRecord& record : records) {
    if (std::find(emitted.begin(), emitted.end(), record.name) != emitted.end()) {
      continue;
    }
    emitted.push_back(record.name);
    if (!emit(lines, record, "mono_" + record.name + "_us", "us",
              static_cast<double>(record.mono_us))) {
      return std::string();
    }
    if (!emit(lines, record, "step_" + record.name, "step", static_cast<double>(record.step))) {
      return std::string();
    }
  }
  const MilestoneRecord& anchor = records.front();
  for (const DerivedMetric& derived : kDerivedMonotonic) {
    std::size_t start_index = 0;
    std::size_t end_index = 0;
    if (!first(derived.start, start_index) || !first(derived.end, end_index)) {
      // 缺任一端就不产生这条指标：写 0 会让“没有发生”看起来像“耗时为零”。
      continue;
    }
    const double value =
        static_cast<double>(records[end_index].mono_us - records[start_index].mono_us);
    if (!emit(lines, records[end_index], derived.name, derived.unit, value)) {
      return std::string();
    }
  }
  if (frozen && !emit(lines, records.back(), "mono_run_total_us", "us",
                      static_cast<double>(total_us))) {
    return std::string();
  }
  if (!emit(lines, anchor, "step_total", "step", static_cast<double>(records.size()))) {
    return std::string();
  }
  if (!emit(lines, anchor, "step_audio_frames", "frame", static_cast<double>(audio_frames))) {
    return std::string();
  }
  if (frame_ms > 0) {
    // 推导值：帧数 × 帧长。它由音频契约（16 kHz/单声道/20 ms）决定，因此是设计约束下的
    // 应播时长，不是实测播放时长。
    const double duration_ms = static_cast<double>(audio_frames) * static_cast<double>(frame_ms);
    if (!emit(lines, anchor, "step_audio_duration_ms", "ms", duration_ms)) {
      return std::string();
    }
  }
  return lines;
}

std::string RunEvidenceRecorder::summary_markdown() {
  std::vector<MilestoneRecord> records;
  bool frozen = false;
  RunOutcome outcome;
  std::string start_time;
  std::string end_time;
  std::int64_t end_us = 0;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    records = milestones_;
    frozen = finished_;
    outcome = outcome_;
    start_time = start_time_;
    end_time = end_time_;
    end_us = end_us_;
  }
  const auto first = [&records](std::string_view name, std::size_t& index) {
    for (std::size_t position = 0; position < records.size(); ++position) {
      if (records[position].name == name) {
        index = position;
        return true;
      }
    }
    return false;
  };

  std::string text;
  text += "# Mock 运行证据摘要\n\n";
  text += "本文件与同目录的 `run-manifest.json`、`events.jsonl`、`metrics.jsonl`、"
          "`protocol.jsonl` 来自同一批里程碑记录，因此它们不会互相矛盾。\n\n";

  text += "## 运行身份\n\n";
  text += "| 字段 | 值 |\n|---|---|\n";
  text += "| 运行标识 | " + Code(config_.run_id) + " |\n";
  text += "| profile | " + Code(config_.profile) + " |\n";
  text += "| 运行起点（UTC） | " + Code(start_time) + " |\n";
  text += "| 运行终点（UTC） | " + Code(frozen ? end_time : std::string()) + " |\n";
  text += "| 退出码 | " + Code(std::to_string(outcome.exit_code)) + " |\n";
  text += "| 运行级错误 | " + Code(outcome.error_code) + " |\n";
  text +=
      "| 场景形态与声明一致 | " + Code(outcome.expectation_matched ? "true" : "false") + " |\n";
  text += "| 命令哈希 | " + Code(config_.command_hash) + " |\n";
  text += "| 配置哈希 | " + Code(config_.config_hash) + " |\n";
  text += "| 输入哈希 | " + Code(config_.input_hash) + " |\n";
  text += "| 执行命令 | " + Code(config_.command) + " |\n\n";

  text += "## 环境与版本\n\n";
  text += "| 字段 | 值 |\n|---|---|\n";
  text += "| Git 提交 | " + Code(config_.environment.git_commit) + " |\n";
  text += "| 编译器 | " + Code(config_.environment.compiler) + " |\n";
  text += "| CMake | " + Code(config_.environment.cmake) + " |\n";
  text += "| 运行时 | " + Code(config_.environment.runtime) + " |\n";
  text += "| 驱动 | " + Code(config_.environment.driver) + " |\n";
  text += "| 模型 | " + Code(config_.environment.model) + " |\n";
  text += "| 设备 | " + Code(config_.environment.device) + " |\n\n";

  text += "## 里程碑\n\n";
  text += "步数是调度步数：本次运行内按提交顺序递增，与主机和墙钟无关，可逐字节复现。\n";
  text += "单调时间自 `run_start` 起算，是进程内实测值，只对本次运行所在的主机与负载成立。\n\n";
  text += "| 步 | 里程碑 | 自 run_start 起的单调时间（us） | 来源 |\n";
  text += "|---:|---|---:|---|\n";
  for (const MilestoneRecord& record : records) {
    text += "| " + std::to_string(record.step) + " | " + Code(record.name) + " | " +
            std::to_string(record.mono_us) + " | " + Code(record.source) + " |\n";
  }
  std::string missing;
  for (std::size_t index = 0; index < kAllMilestoneCount; ++index) {
    const char* const name = kAllMilestones[index];
    std::size_t position = 0;
    if (first(name, position)) {
      continue;
    }
    std::string reason;
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      reason = unmeasured_reason_locked(name);
    }
    missing += "| " + Code(name) + " | 未测量：" + EscapeCell(reason) + " |\n";
  }
  if (!missing.empty()) {
    text += "\n未记录的候选里程碑（未测量不等于耗时为 0）：\n\n";
    text += "| 里程碑 | 状态 |\n|---|---|\n";
    text += missing;
  }

  text += "\n## 指标\n\n";
  text += "| 指标 | 值 | 单位 | 口径（起点 → 终点） |\n|---|---:|---|---|\n";
  // 指标表与 metrics.jsonl 同源：这里按同样的规则（同名里程碑取首次出现）复算一遍并附
  // 上口径说明，避免读者只拿到一堆没有起止定义的数值，也避免两张表给出不同的值。
  std::vector<std::string> listed;
  for (const MilestoneRecord& record : records) {
    if (std::find(listed.begin(), listed.end(), record.name) != listed.end()) {
      continue;
    }
    listed.push_back(record.name);
    text += "| " + Code("mono_" + record.name + "_us") + " | " +
            std::to_string(record.mono_us) + " | us | " +
            EscapeCell(MetricScope("mono_" + record.name + "_us")) + " |\n";
    text += "| " + Code("step_" + record.name) + " | " + std::to_string(record.step) +
            " | step | " + EscapeCell(MetricScope("step_" + record.name)) + " |\n";
  }
  for (const DerivedMetric& derived : kDerivedMonotonic) {
    std::size_t start_index = 0;
    std::size_t end_index = 0;
    if (!first(derived.start, start_index) || !first(derived.end, end_index)) {
      continue;
    }
    const std::int64_t value = records[end_index].mono_us - records[start_index].mono_us;
    text += "| " + Code(derived.name) + " | " + std::to_string(value) + " | " +
            std::string(derived.unit) + " | " + EscapeCell(MetricScope(derived.name)) + " |\n";
  }
  if (frozen) {
    text += "| " + Code("mono_run_total_us") + " | " + std::to_string(end_us) + " | us | " +
            EscapeCell(MetricScope("mono_run_total_us")) + " |\n";
  }
  text += "| " + Code("step_total") + " | " + std::to_string(records.size()) + " | step | " +
          EscapeCell(MetricScope("step_total")) + " |\n";
  text += "| " + Code("step_audio_frames") + " | " + std::to_string(outcome.audio_frames) +
          " | frame | " + EscapeCell(MetricScope("step_audio_frames")) + " |\n";
  if (config_.frame_ms > 0) {
    const double duration_ms =
        static_cast<double>(outcome.audio_frames) * static_cast<double>(config_.frame_ms);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.0f", duration_ms);
    text += "| " + Code("step_audio_duration_ms") + " | " + std::string(buffer) + " | ms | " +
            EscapeCell(MetricScope("step_audio_duration_ms")) + " |\n";
  }

  text += "\n## 取消阶段\n\n";
  std::size_t accepted = 0;
  if (!first(milestone::kCancelAccepted, accepted)) {
    text += "本次运行没有发生取消，因此没有可报告的取消阶段。\n";
  } else {
    text += "取消阶段按固定顺序提交，顺序不变量见 `core/runtime/interaction_contract.hpp`：\n\n";
    for (const char* const phase : kCancelPhases) {
      std::size_t index = 0;
      if (first(phase, index)) {
        text += "- " + Code(phase) + "：步 " + std::to_string(records[index].step) +
                "，自 run_start 起 " + std::to_string(records[index].mono_us) + " us\n";
      } else {
        text += "- " + Code(phase) + "：未记录（阶段缺失，按不变量不成立处理）\n";
      }
    }
    std::size_t cleared = 0;
    std::size_t terminal = 0;
    if (first(milestone::kPlaybackCleared, cleared)) {
      text += "\n受理到播放清理的单调间隔：`mono_cancel_cleared_us` = " +
              std::to_string(records[cleared].mono_us - records[accepted].mono_us) + " us。\n";
    }
    if (first(milestone::kTerminalCancelled, terminal)) {
      text += "受理到取消终态的单调间隔：`mono_cancel_terminal_us` = " +
              std::to_string(records[terminal].mono_us - records[accepted].mono_us) + " us。\n";
    }
    text += "\n本版本的播放清理是同步且非阻塞的，同一线性化点上提交的阶段会得到相同的单调"
            "时间；时间差为 0 表示它们确实在同一时刻提交，而不是阶段丢失。\n";
  }

  text += "\n## 口径与限制\n\n";
  text += "- 单调时间族（`mono_*`）取自进程内单调时钟，只对本次运行所在的主机、构建与负载"
          "成立；它**不是**真实设备延迟、吞吐或稳定性结论。\n";
  text += "- 调度步数族（`step_*`）由记录器按提交顺序分配，与主机和墙钟无关，可用于核对因果"
          "顺序；`step_audio_duration_ms` 由帧数 × 帧长推导，是音频契约下的应播时长，"
          "不是实测播放时长。\n";
  text += "- 未测量项不等于 0：某条指标只在其起止里程碑都出现时才产生。\n";
  text += "- 逐帧 PCM 下行不属于本版本请求入口的交付内容，因此 `protocol.jsonl` 中没有 PCM "
          "事件；音频帧数取自会话自己的运行记录。\n";
  return text;
}

}  // namespace nexweave::observability

// 运行证据记录器的单元测试：验收"证据层自己的不变量"，不涉及会话、线程或真实时钟。
//
// 与集成测试的分工：集成测试验收"一次真实 Mock 运行产生了哪些里程碑"；本文件用可控时钟与
// 直接投递的观察者回调验收记录器本身——步数分配、首次出现语义、未测量与零的区分、
// 三类产物的编解码自洽，以及"并发提交不会重复分配步数"。
//
// 保护的不变量（每条在断言旁注明）：
//   1. 步数是调度步数：从 0 开始、按提交顺序递增、与主机和时钟无关。
//   2. 单调时间是实测值：完全由注入的时钟决定，因此可以精确断言而不是容差比较。
//   3. 事件流保留全部出现，指标取首次出现；两者不会互相覆盖。
//   4. 未测量不等于 0：起止里程碑缺任一端就不产生该指标。
//   5. finish 幂等且冻结：之后的写入一律被丢弃，run_end 只出现一次。
//   6. 三类产物都能被可观测层自己的解码器原样解回，字段集合与校验语义一致。
//   7. 里程碑入口可并发调用，步数不会重复。
#include "../test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "run_evidence.hpp"

using nexweave::observability::decode_event;
using nexweave::observability::decode_manifest;
using nexweave::observability::decode_metric;
using nexweave::observability::IMonotonicClock;
using nexweave::observability::milestone::kCancelAccepted;
using nexweave::observability::milestone::kFirstPcm;
using nexweave::observability::milestone::kFirstToken;
using nexweave::observability::milestone::kGenerationDone;
using nexweave::observability::milestone::kGenerationStarted;
using nexweave::observability::milestone::kPlaybackCleared;
using nexweave::observability::milestone::kPlaybackDone;
using nexweave::observability::milestone::kPlaybackStart;
using nexweave::observability::milestone::kRunEnd;
using nexweave::observability::milestone::kRunStart;
using nexweave::observability::milestone::kSynthesisDone;
using nexweave::observability::milestone::kTerminalCancelled;
using nexweave::observability::MilestoneRecord;
using nexweave::observability::RunEvidenceConfig;
using nexweave::observability::RunEvidenceRecorder;
using nexweave::observability::RunOutcome;
using nexweave::runtime::ActivityMarker;

namespace {

// 手动单调时钟：证据层的时间全部由它决定，因此"实测时间"在单元测试里也是确定值。
// 它把"时间从哪来"变成显式输入，使时间相关的分支可以被精确断言，而不是靠容差比较。
class ManualMonotonicClock final : public IMonotonicClock {
 public:
  explicit ManualMonotonicClock(std::int64_t start_us) noexcept : now_us_(start_us) {}
  std::int64_t now_us() const noexcept override { return now_us_; }
  void advance(std::int64_t delta_us) noexcept { now_us_ += delta_us; }

 private:
  std::int64_t now_us_;
};

RunEvidenceConfig MakeConfig() {
  RunEvidenceConfig config;
  config.run_id = "mock-normal";
  config.profile = "mock";
  config.command = "nexweave_mock_profile --scenario normal";
  config.command_hash = "fnv1a64:0000000000000001";
  config.config_hash = "fnv1a64:0000000000000002";
  config.input_hash = "fnv1a64:0000000000000003";
  config.request_id = "req-mock-normal-start";
  config.session_id = "session-mock-normal";
  config.environment.git_commit = "0123456789abcdef";
  config.environment.compiler = "gcc 11.4.0";
  config.environment.cmake = "3.22.1";
  config.environment.runtime = "nexweave-mock-profile/normal";
  config.environment.driver = "none";
  config.environment.model = "fake";
  config.environment.device = "none";
  config.frame_ms = 20;
  return config;
}

std::size_t CountName(const std::vector<MilestoneRecord>& records, const std::string& name) {
  std::size_t count = 0;
  for (const MilestoneRecord& record : records) {
    if (record.name == name) {
      ++count;
    }
  }
  return count;
}

// 取同名指标的出现次数与首个取值；逐行解码的规则来自公共辅助，本函数只多数一次出现次数。
std::size_t MetricSamples(const std::string& metrics, const std::string& name, double* first_value) {
  std::size_t count = 0;
  for (const std::string& line : nexweave::test::jsonl_lines(metrics)) {
    const auto metric = decode_metric(line);
    if (metric.ok() && metric.value->name == name) {
      if (count == 0 && first_value != nullptr) {
        *first_value = metric.value->value;
      }
      ++count;
    }
  }
  return count;
}

// 保护不变量 1 与 2：步数由提交顺序决定，单调时间由注入时钟决定。两者必须分别正确，
// 因为"可复现的顺序"和"实测的耗时"是两种不同性质的证据。
void TestStepsAreSubmissionOrderAndMonoFollowsInjectedClock() {
  ManualMonotonicClock clock(1000000);
  RunEvidenceRecorder recorder(MakeConfig(), clock);
  CHECK(recorder.milestone_count() == 1);

  clock.advance(120);
  recorder.on_marker(ActivityMarker::kGenerationStarted, 1);
  clock.advance(30);
  // 会话侧第一帧交付给播放组件：它是 first_pcm，不是 playback_start（设备侧边界）。
  recorder.on_marker(ActivityMarker::kPlaybackStarted, 1);
  clock.advance(10);
  recorder.on_token_delivered("hello");
  clock.advance(40);
  recorder.on_marker(ActivityMarker::kGenerationDone, 1);
  clock.advance(5);
  recorder.on_marker(ActivityMarker::kSynthesisDone, 1);
  clock.advance(1);
  recorder.on_marker(ActivityMarker::kPlaybackDone, 1);

  bool found = false;
  // step_of / mono_us_of 返回的是取值：起点恰好为 0，因此不能把它当成布尔写进短路条件。
  CHECK(recorder.has_milestone(kRunStart));
  CHECK(recorder.step_of(kRunStart, found) == 0 && found);
  CHECK(recorder.step_of(kGenerationStarted, found) && found);
  CHECK(recorder.step_of(kGenerationStarted, found) == 1);
  CHECK(recorder.step_of(kFirstPcm, found) && recorder.step_of(kFirstPcm, found) == 2);
  CHECK(recorder.step_of(kFirstToken, found) && recorder.step_of(kFirstToken, found) == 3);
  CHECK(recorder.step_of(kGenerationDone, found) && recorder.step_of(kGenerationDone, found) == 4);
  CHECK(recorder.step_of(kSynthesisDone, found) && recorder.step_of(kSynthesisDone, found) == 5);
  CHECK(recorder.step_of(kPlaybackDone, found) && recorder.step_of(kPlaybackDone, found) == 6);
  // 未出现的里程碑：步数查询必须报告"没找到"，而不是返回 0 让调用方误以为它在起点。
  CHECK(!recorder.step_of(kPlaybackStart, found));
  CHECK(!found);

  CHECK(recorder.mono_us_of(kRunStart, found) == 0 && found);
  CHECK(recorder.mono_us_of(kGenerationStarted, found) &&
        recorder.mono_us_of(kGenerationStarted, found) == 120);
  CHECK(recorder.mono_us_of(kFirstPcm, found) && recorder.mono_us_of(kFirstPcm, found) == 150);
  CHECK(recorder.mono_us_of(kFirstToken, found) && recorder.mono_us_of(kFirstToken, found) == 160);
  CHECK(recorder.mono_us_of(kGenerationDone, found) &&
        recorder.mono_us_of(kGenerationDone, found) == 200);

  // 顺序与时钟无关：即使两次读数的差值很小，步序仍然严格递增且连续。
  const std::vector<MilestoneRecord> records = recorder.milestones();
  for (std::size_t index = 1; index < records.size(); ++index) {
    CHECK(records[index].step == index);
    CHECK(records[index].mono_us >= records[index - 1].mono_us);
  }
  CHECK(records.front().name == kRunStart);
}

// 保护不变量 3：事件流保留全部出现，指标取首次出现。多轮会话里同一个里程碑会再次出现，
// 若指标跟着最后一次漂移，"首 token 延迟"就会随轮数变化，而它早已不是同一个问题。
void TestEventsKeepEveryOccurrenceAndMetricsUseTheFirst() {
  ManualMonotonicClock clock(0);
  RunEvidenceRecorder recorder(MakeConfig(), clock);
  clock.advance(100);
  recorder.on_marker(ActivityMarker::kGenerationStarted, 1);
  clock.advance(100);
  recorder.on_marker(ActivityMarker::kGenerationStarted, 2);
  clock.advance(100);
  recorder.on_marker(ActivityMarker::kCancelAccepted, 2);

  const std::vector<MilestoneRecord> records = recorder.milestones();
  CHECK(CountName(records, kGenerationStarted) == 2);
  bool found = false;
  CHECK(recorder.step_of(kGenerationStarted, found) == 1);
  CHECK(recorder.mono_us_of(kGenerationStarted, found) == 100);

  double value = 0.0;
  CHECK(MetricSamples(recorder.metrics_jsonl(), "mono_generation_started_us", &value) == 1);
  CHECK(value == 100.0);
  // 事件流保留两次出现，指标只留首次：两者不互相覆盖，也不互相替代。
  CHECK(nexweave::test::jsonl_lines(recorder.events_jsonl()).size() == records.size());
}

// 保护不变量 4：未测量不等于 0。首 token 没发生时，mono_first_token_us 既不出现 0，
// 也不出现任何值；摘要里必须给出"未测量"的具体原因。
void TestMissingMilestonesProduceNoMetricAndAReadableReason() {
  ManualMonotonicClock clock(0);
  RunEvidenceRecorder recorder(MakeConfig(), clock);
  clock.advance(50);
  recorder.on_marker(ActivityMarker::kGenerationStarted, 1);
  clock.advance(10);
  recorder.on_marker(ActivityMarker::kPlaybackStarted, 1);
  clock.advance(10);
  recorder.on_first_frame_written();
  clock.advance(10);
  recorder.on_marker(ActivityMarker::kGenerationDone, 1);
  clock.advance(10);
  recorder.on_marker(ActivityMarker::kSynthesisDone, 1);
  clock.advance(10);
  recorder.on_marker(ActivityMarker::kPlaybackDone, 1);

  RunOutcome outcome;
  outcome.exit_code = 0;
  outcome.audio_frames = 4;
  recorder.finish(outcome);

  const std::string metrics = recorder.metrics_jsonl();
  CHECK(MetricSamples(metrics, "mono_first_token_us", nullptr) == 0);
  CHECK(MetricSamples(metrics, "mono_cancel_accepted_us", nullptr) == 0);
  CHECK(MetricSamples(metrics, "mono_playback_start_us", nullptr) == 1);
  CHECK(MetricSamples(metrics, "mono_cancel_cleared_us", nullptr) == 0);
  // 重叠成立：设备侧播放开始（步 3）早于文本定稿（步 5）。
  CHECK(recorder.overlap_proven());

  double frames = 0.0;
  double duration = 0.0;
  CHECK(MetricSamples(metrics, "step_audio_frames", &frames) == 1);
  CHECK(MetricSamples(metrics, "step_audio_duration_ms", &duration) == 1);
  CHECK(frames == 4.0);
  CHECK(duration == 80.0);

  const std::string summary = recorder.summary_markdown();
  CHECK(summary.find("未测量") != std::string::npos);
  CHECK(summary.find("first_token") != std::string::npos);
  // 推导指标必须自带口径说明：读者不能把它读成实测播放时长。
  CHECK(summary.find("不是实测播放时长") != std::string::npos);
}

// 保护不变量 5：finish 幂等且冻结。第一次调用确定运行终点与结论，之后的写入（包括迟到
// 的设备回调）一律被丢弃，否则摘要与指标会描述两份不同的快照。
void TestFinishIsIdempotentAndFreezesTheSnapshot() {
  ManualMonotonicClock clock(0);
  RunEvidenceRecorder recorder(MakeConfig(), clock);
  clock.advance(200);
  recorder.on_marker(ActivityMarker::kTerminalCancelled, 1);

  RunOutcome outcome;
  outcome.exit_code = 1;
  outcome.error_code = "timeout";
  outcome.expectation_matched = false;
  outcome.audio_frames = 2;
  clock.advance(300);
  recorder.finish(outcome);
  const std::size_t after_finish = recorder.milestone_count();
  CHECK(recorder.finished());

  // 第二次 finish 与冻结后的迟到回调都不得改变快照。
  clock.advance(1000);
  recorder.finish(outcome);
  recorder.on_marker(ActivityMarker::kPlaybackStarted, 1);
  recorder.on_first_frame_written();
  recorder.on_token_delivered("late");
  CHECK(recorder.milestone_count() == after_finish);

  const std::vector<MilestoneRecord> records = recorder.milestones();
  CHECK(records.back().name == kRunEnd);
  CHECK(records.back().mono_us == 500);
  CHECK(CountName(records, kRunEnd) == 1);

  const std::string summary = recorder.summary_markdown();
  CHECK(summary.find("`timeout`") != std::string::npos);
  CHECK(summary.find("| 场景形态与声明一致 | `false` |") != std::string::npos);
}

// 保护不变量 6：三类产物都能被可观测层自己的解码器解回，且字段与提交的事实一致。
// 只断言"文本里有某个子串"会让字段改名或编码漂移悄悄通过，因此这里逐行回解。
void TestArtifactsRoundTripThroughTheObservabilityDecoders() {
  ManualMonotonicClock clock(0);
  RunEvidenceRecorder recorder(MakeConfig(), clock);
  clock.advance(10);
  recorder.on_marker(ActivityMarker::kGenerationStarted, 1);
  clock.advance(10);
  recorder.on_marker(ActivityMarker::kPlaybackStarted, 1);
  clock.advance(10);
  recorder.on_first_frame_written();
  clock.advance(10);
  // 取消受理是"受理 → 播放清理"这条间隔指标的起点；缺了它，指标不产生——这正是
  // "未测量不等于 0"的落点，因此本用例必须真的提交它。
  recorder.on_marker(ActivityMarker::kCancelAccepted, 1);
  // 不推进时钟：同一次取消的各阶段在同一线性化点上提交，时间差因此是合法的 0。
  recorder.on_marker(ActivityMarker::kPlaybackCleared, 1);
  clock.advance(10);
  recorder.on_marker(ActivityMarker::kTerminalCancelled, 1);
  RunOutcome outcome;
  outcome.exit_code = 0;
  outcome.audio_frames = 1;
  recorder.finish(outcome);

  const auto manifest = decode_manifest(recorder.manifest_json());
  CHECK(manifest.ok());
  CHECK(manifest.value->run_id == "mock-normal");
  CHECK(manifest.value->profile == "mock");
  CHECK(manifest.value->git_commit == "0123456789abcdef");
  CHECK(manifest.value->compiler == "gcc 11.4.0");
  CHECK(manifest.value->cmake == "3.22.1");
  CHECK(manifest.value->config_hash == "fnv1a64:0000000000000002");
  CHECK(manifest.value->input_hash == "fnv1a64:0000000000000003");
  CHECK(manifest.value->command == "nexweave_mock_profile --scenario normal");
  CHECK(manifest.value->start_time != "none");
  CHECK(manifest.value->end_time != "none");

  const std::vector<std::string> event_lines = nexweave::test::jsonl_lines(recorder.events_jsonl());
  CHECK(event_lines.size() == recorder.milestone_count());
  std::uint64_t previous_sequence = 0;
  bool first_event = true;
  for (const std::string& line : event_lines) {
    const auto event = decode_event(line);
    CHECK(event.ok());
    CHECK(event.value->request_id == "req-mock-normal-start");
    CHECK(event.value->session_id == "session-mock-normal");
    CHECK(!event.value->name.empty());
    CHECK(first_event || event.value->sequence > previous_sequence);
    previous_sequence = event.value->sequence;
    first_event = false;
  }
  // 起点事件携带三个关联哈希：清单的字段集合是 v1 契约，加字段需要改协议版本，因此
  // 命令哈希与两个已有哈希并列放在这里。
  const auto run_start = decode_event(event_lines.front());
  CHECK(run_start.ok());
  CHECK(run_start.value->name == kRunStart);
  CHECK(run_start.value->attributes.at("command_hash") == "fnv1a64:0000000000000001");
  CHECK(run_start.value->attributes.at("config_hash") == "fnv1a64:0000000000000002");
  CHECK(run_start.value->attributes.at("input_hash") == "fnv1a64:0000000000000003");
  CHECK(run_start.value->attributes.at("frame_ms") == "20");
  // 契约字段是毫秒，微秒读数放在属性里：毫秒分辨率不足以区分相邻阶段。
  CHECK(run_start.value->attributes.at("mono_us") == "0");
  // 运行结论写在终点事件上，使"这一次跑成什么样"与"跑到哪一步"在同一条记录里可查。
  const auto run_end = decode_event(event_lines.back());
  CHECK(run_end.ok());
  CHECK(run_end.value->name == kRunEnd);
  CHECK(run_end.value->attributes.at("exit_code") == "0");
  CHECK(run_end.value->attributes.at("error_code") == "none");
  CHECK(run_end.value->attributes.at("audio_frames") == "1");

  const std::vector<std::string> metric_lines = nexweave::test::jsonl_lines(recorder.metrics_jsonl());
  CHECK(!metric_lines.empty());
  bool saw_cancel_interval = false;
  for (const std::string& line : metric_lines) {
    const auto metric = decode_metric(line);
    CHECK(metric.ok());
    CHECK(!metric.value->unit.empty());
    CHECK(metric.value->request_id == "req-mock-normal-start");
    if (metric.value->name == "mono_cancel_cleared_us") {
      saw_cancel_interval = true;
      // 同一次取消的各阶段在同一线性化点上提交，因此间隔可以是 0——0 是合法耗时，
      // 与"没有发生"用"指标是否存在"区分。
      CHECK(metric.value->unit == "us");
      CHECK(metric.value->value == 0.0);
    }
  }
  CHECK(saw_cancel_interval);
}

// 保护不变量 7：里程碑入口可并发调用。步数分配在锁内完成，因此并发提交既不会丢事件，
// 也不会让两个事件拿到同一个步数——后者会让"顺序即因果"这条判据失效。
void TestConcurrentMilestoneEntryPointsDoNotShareSteps() {
  ManualMonotonicClock clock(0);
  RunEvidenceRecorder recorder(MakeConfig(), clock);
  constexpr int kPerThread = 64;
  std::thread first([&recorder]() {
    for (int index = 0; index < kPerThread; ++index) {
      recorder.on_marker(ActivityMarker::kGenerationStarted, 1);
    }
  });
  std::thread second([&recorder]() {
    for (int index = 0; index < kPerThread; ++index) {
      recorder.on_marker(ActivityMarker::kOldOutputBlocked, 1);
    }
  });
  first.join();
  second.join();
  recorder.finish(RunOutcome{});

  const std::vector<MilestoneRecord> records = recorder.milestones();
  CHECK(records.size() == static_cast<std::size_t>(2 * kPerThread) + 2);
  std::vector<std::uint64_t> steps;
  steps.reserve(records.size());
  for (const MilestoneRecord& record : records) {
    steps.push_back(record.step);
  }
  std::sort(steps.begin(), steps.end());
  for (std::size_t index = 0; index < steps.size(); ++index) {
    CHECK(steps[index] == index);
  }
}

// 保护不变量 2 与 4：重叠判据由步序决定，因此"生成结束前已经开始播放"这条结论与主机无关。
// 文本先定稿再开始播放（L0/L1 串行直答）时，同一个函数必须返回假，且领先量为负。
void TestOverlapIsDecidedByStepOrderNotByWallClock() {
  ManualMonotonicClock clock(0);
  RunEvidenceRecorder recorder(MakeConfig(), clock);
  recorder.on_marker(ActivityMarker::kGenerationStarted, 1);
  recorder.on_marker(ActivityMarker::kGenerationDone, 1);
  recorder.on_first_frame_written();
  recorder.on_marker(ActivityMarker::kPlaybackStarted, 1);
  CHECK(!recorder.overlap_proven());

  double lead = 0.0;
  CHECK(MetricSamples(recorder.metrics_jsonl(), "mono_overlap_lead_us", &lead) == 1);
  CHECK(lead == 0.0);
  // 步序上生成先定稿：同一个领先量为 0 的读数在这里意味着"没有重叠"，而不是"重叠恰好为 0"，
  // 因此判据必须同时使用步序，而不是只看时间差。
  bool found = false;
  CHECK(recorder.step_of(kGenerationDone, found) < recorder.step_of(kPlaybackStart, found));
}

}  // namespace

int main() {
  TestStepsAreSubmissionOrderAndMonoFollowsInjectedClock();
  TestEventsKeepEveryOccurrenceAndMetricsUseTheFirst();
  TestMissingMilestonesProduceNoMetricAndAReadableReason();
  TestFinishIsIdempotentAndFreezesTheSnapshot();
  TestArtifactsRoundTripThroughTheObservabilityDecoders();
  TestConcurrentMilestoneEntryPointsDoNotShareSteps();
  TestOverlapIsDecidedByStepOrderNotByWallClock();
  std::printf("run_evidence: 全部检查通过\n");
  return 0;
}

// Mock 运行证据的集成夹具：验收"一次真实运行到底留下了哪些可核对的事实"。
//
// 分工：mock_profile_test.cpp 验收场景选择、确定性与清理账目；本文件验收运行证据本身——
// 五份产物是否齐备、里程碑与指标是否覆盖 ticket 要求的时刻、重叠与取消是否真的被证明、
// 未测量的时间点是否被如实标注而不是写成 0。
//
// 保护的不变量（每条在断言旁注明）：
//   1. 一次运行产生五份产物，内容同源：清单、事件流、指标流与摘要都由同一批里程碑派生。
//   2. 三个局部完成时刻（文本定稿、合成结束、播放结束）在成功路径上都被记录；
//      取消场景记录五个取消阶段，且顺序与交互契约一致。
//   3. 重叠由调度步数证明，而不是靠墙钟：L2 路径上"播放已开始、生成未结束"必须成立。
//   4. 调度步数与主机无关：同一配置重复运行得到逐字节一致的里程碑序列与 step 指标。
//   5. 未测量不等于 0：缺失的里程碑既不出现在指标流里，也在摘要里写明原因。
//   6. 产物策略决定产物归宿：transient 之后目录为空，retained 之后文件仍在且账目记为
//      "有意保留"而不是"没删掉"。
//
// 关于时间口径：单调时间族是实测值，测试只断言它与步序一致（单调不减、同一步序先后有序），
// 不断言具体数值——把实测值写进断言就等于把"这台机器这次跑多快"当成契约。
#include "../test_support.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "mock_profile.hpp"

using nexweave::app::MockProfileArtifactPolicy;
using nexweave::app::MockProfileConfig;
using nexweave::app::MockProfileResult;
using nexweave::app::MockProfileScenario;
using nexweave::app::mock_profile_scenario_slug;
using nexweave::app::run_mock_profile;
using nexweave::app::to_string;
using nexweave::domain::ErrorCode;
using nexweave::observability::MilestoneRecord;

namespace {

const MockProfileScenario kAllScenarios[] = {
    MockProfileScenario::kNormal, MockProfileScenario::kSlowConsumer,
    MockProfileScenario::kCancel, MockProfileScenario::kFault};

// 每次运行一个独立目录，目录名带进程号，使并行运行的多个测试进程互不覆盖。
std::string EvidenceDir(const std::string& label) {
  return (std::filesystem::temp_directory_path() /
          ("nexweave-evidence-" + std::to_string(::getpid()) + "-" + label))
      .string();
}

MockProfileConfig ConfigFor(MockProfileScenario scenario, const std::string& output_dir) {
  MockProfileConfig config;
  config.scenario = scenario;
  config.stream_id = std::string("mock-") + mock_profile_scenario_slug(scenario);
  config.output_dir = output_dir;
  config.artifact_policy =
      output_dir.empty() ? MockProfileArtifactPolicy::kTransient : MockProfileArtifactPolicy::kRetained;
  if (scenario == MockProfileScenario::kCancel) {
    config.cancel_after_pcm_events = 1;
  }
  if (scenario == MockProfileScenario::kSlowConsumer) {
    config.drain_budget_bytes = 8;
  }
  return config;
}

bool ReadFile(const std::string& path, std::string& content) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return false;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  content = buffer.str();
  return true;
}

const MilestoneRecord* Find(const std::vector<MilestoneRecord>& records, const std::string& name) {
  for (const MilestoneRecord& record : records) {
    if (record.name == name) {
      return &record;
    }
  }
  return nullptr;
}

std::vector<std::string> Names(const std::vector<MilestoneRecord>& records) {
  std::vector<std::string> names;
  names.reserve(records.size());
  for (const MilestoneRecord& record : records) {
    names.push_back(record.name);
  }
  return names;
}

// 按行拆分 JSONL，忽略结尾换行造成的空行。
std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t position = 0;
  while (position < text.size()) {
    const std::size_t newline = text.find('\n', position);
    if (newline == std::string::npos) {
      lines.push_back(text.substr(position));
      break;
    }
    if (newline > position) {
      lines.push_back(text.substr(position, newline - position));
    }
    position = newline + 1;
  }
  return lines;
}

// 指标流里某个名称的取值；找不到返回假。用解码而不是子串搜索：字段顺序由 JSON 库决定。
bool MetricValue(const std::string& metrics, const std::string& name, double& value) {
  for (const std::string& line : Lines(metrics)) {
    const auto metric = nexweave::observability::decode_metric(line);
    if (metric.ok() && metric.value->name == name) {
      value = metric.value->value;
      return true;
    }
  }
  return false;
}

bool HasMetric(const std::string& metrics, const std::string& name) {
  double ignored = 0.0;
  return MetricValue(metrics, name, ignored);
}

// 只保留 step_ 族指标，用于比较两次运行的"可复现部分"。mono_ 族是实测值，本来就不该相同。
std::string StepMetrics(const std::string& metrics) {
  std::string selected;
  for (const std::string& line : Lines(metrics)) {
    const auto metric = nexweave::observability::decode_metric(line);
    if (metric.ok() && metric.value->name.rfind("step_", 0) == 0) {
      selected += std::to_string(metric.value->value) + " " + metric.value->name + "\n";
    }
  }
  return selected;
}

// 保护不变量 1 与 2、4：四个场景各自产生五份产物，成功路径记录三类完成时刻，
// 取消路径记录五个取消阶段，故障路径在开始任何轮次之前就收敛。
void TestEachScenarioProducesCompleteEvidence() {
  for (const MockProfileScenario scenario : kAllScenarios) {
    const std::string dir = EvidenceDir(std::string("shape-") + to_string(scenario));
    std::filesystem::remove_all(dir);
    const MockProfileResult result = run_mock_profile(ConfigFor(scenario, dir));
    const std::string label = std::string("场景 ") + to_string(scenario);

    CHECK_MESSAGE(result.exit_code == 0, label + " 应当按契约完成：" + result.summary_json);
    CHECK_MESSAGE(result.ledger.artifacts_written == 5, label + " 应当产生五份产物");
    CHECK_MESSAGE(result.ledger.artifacts_retained, label + " 的产物应当按策略保留");
    CHECK_MESSAGE(!result.ledger.artifacts_removed, label + " 的产物不应该被删除");
    CHECK_MESSAGE(result.ledger.quiesced(), label + " 结束时不应当残留未交还的资源");

    const char* const names[] = {"run-manifest.json", "events.jsonl", "metrics.jsonl",
                                 "protocol.jsonl", "summary.md"};
    for (const char* const name : names) {
      std::string content;
      CHECK_MESSAGE(ReadFile(dir + "/" + name, content), label + " 缺少产物 " + name);
      CHECK_MESSAGE(!content.empty(), label + " 的产物为空：" + name);
    }

    std::string summary;
    CHECK(ReadFile(dir + "/summary.md", summary));
    CHECK_MESSAGE(summary.find("## 里程碑") != std::string::npos, label + " 摘要缺少里程碑表");
    CHECK_MESSAGE(summary.find("## 指标") != std::string::npos, label + " 摘要缺少指标表");
    CHECK_MESSAGE(summary.find("口径与限制") != std::string::npos, label + " 摘要缺少口径说明");

    const std::vector<MilestoneRecord>& records = result.milestones;
    CHECK_MESSAGE(records.front().name == "run_start", label + " 的第一个里程碑必须是运行起点");
    CHECK_MESSAGE(records.back().name == "run_end", label + " 的最后一个里程碑必须是运行终点");
    // 步数即调度步数：连续、从 0 开始、与主机无关。
    for (std::size_t index = 0; index < records.size(); ++index) {
      CHECK_MESSAGE(records[index].step == index, label + " 的调度步数必须连续");
    }
    // 单调时间自起点起算，因此必须单调不减；它不需要严格递增（同一线性化点上的阶段同时提交）。
    for (std::size_t index = 1; index < records.size(); ++index) {
      CHECK_MESSAGE(records[index].mono_us >= records[index - 1].mono_us,
                    label + " 的单调时间必须不减");
    }

    if (scenario == MockProfileScenario::kNormal) {
      // L1 直答：文本定稿早于播放开始，三个局部完成都出现。
      CHECK(Find(records, "generation_done") != nullptr);
      CHECK(Find(records, "synthesis_done") != nullptr);
      CHECK(Find(records, "playback_done") != nullptr);
      CHECK(Find(records, "terminal_succeeded") != nullptr);
      CHECK(!result.overlap_proven);
      CHECK(!HasMetric(result.metrics_jsonl, "mono_first_token_us"));
    } else if (scenario == MockProfileScenario::kSlowConsumer) {
      // L2：审核后走生成路径，文本定稿晚于播放开始。
      CHECK(Find(records, "first_token") != nullptr);
      CHECK(Find(records, "generation_done") != nullptr);
      CHECK(Find(records, "synthesis_done") != nullptr);
      CHECK(Find(records, "playback_done") != nullptr);
      CHECK(result.overlap_proven);
    } else if (scenario == MockProfileScenario::kCancel) {
      // 取消：五个阶段齐全、顺序与交互契约一致，且终态是取消而不是成功。
      const char* const phases[] = {"cancel_accepted", "old_output_blocked", "execution_exited",
                                    "playback_cleared", "terminal_cancelled"};
      std::uint64_t previous = 0;
      for (const char* const phase : phases) {
        const MilestoneRecord* const record = Find(records, phase);
        CHECK_MESSAGE(record != nullptr, label + " 缺少取消阶段 " + phase);
        CHECK_MESSAGE(record->step > previous, label + " 的取消阶段顺序不正确");
        previous = record->step;
      }
      CHECK(Find(records, "terminal_succeeded") == nullptr);
      CHECK(!result.observed.session_completed);
    } else {
      // 故障：输入在开始任何轮次之前不可用，因此连代际都没有开启。
      CHECK_MESSAGE(Find(records, "generation_started") == nullptr,
                    label + " 不应该产生任何会话阶段");
      CHECK_MESSAGE(!HasMetric(result.metrics_jsonl, "mono_first_pcm_us"),
                    label + " 不应产生首帧指标");
      CHECK_MESSAGE(summary.find("未测量") != std::string::npos, label + " 应当标注未测量项");
    }
    std::filesystem::remove_all(dir);
  }
}

// 保护不变量 3：重叠由调度步数证明。判据是"设备侧播放开始"的步数严格早于"文本定稿"，
// 而不是时间差——确定性夹具整轮不到一毫秒，时间差可能小于时钟分辨率。
void TestOverlapIsProvenByStepOrderAndPositiveLead() {
  const std::string dir = EvidenceDir("overlap");
  std::filesystem::remove_all(dir);
  const MockProfileResult result = run_mock_profile(ConfigFor(MockProfileScenario::kSlowConsumer, dir));

  const MilestoneRecord* const playback = Find(result.milestones, "playback_start");
  const MilestoneRecord* const generation = Find(result.milestones, "generation_done");
  CHECK(playback != nullptr);
  CHECK(generation != nullptr);
  CHECK(playback->step < generation->step);
  CHECK(result.overlap_proven);

  double lead = 0.0;
  CHECK(MetricValue(result.metrics_jsonl, "mono_overlap_lead_us", lead));
  CHECK(lead > 0.0);

  std::string summary;
  CHECK(ReadFile(dir + "/summary.md", summary));
  CHECK(summary.find("mono_overlap_lead_us") != std::string::npos);
  // 口径必须写明正负的含义，否则一个带符号的数字无法自解释。
  CHECK(summary.find("生成结束前已经开始播放") != std::string::npos);
  std::filesystem::remove_all(dir);
}

// 保护不变量 2：取消把"已经写出的帧保留、未播出的部分丢弃"变成可读的事实。
// 打断点设在第 1 帧之后，因此最终写出音频汇的帧数恰好是 1，而不是整段回答。
void TestCancellationKeepsOnlyTheAudioWrittenBeforeTheStop() {
  const std::string dir = EvidenceDir("cancel");
  std::filesystem::remove_all(dir);
  MockProfileConfig config = ConfigFor(MockProfileScenario::kCancel, dir);
  config.cancel_after_pcm_events = 1;
  const MockProfileResult result = run_mock_profile(config);

  CHECK(result.cancel_frames == 1);
  double frames = 0.0;
  CHECK(MetricValue(result.metrics_jsonl, "step_audio_frames", frames));
  CHECK(frames == 1.0);

  std::string summary;
  CHECK(ReadFile(dir + "/summary.md", summary));
  CHECK(summary.find("## 取消阶段") != std::string::npos);
  // 五个阶段逐一列出；缺任何一个都应该在摘要里写成"阶段缺失"，而不是静默省略。
  const char* const phases[] = {"`cancel_accepted`", "`old_output_blocked`", "`execution_exited`",
                                "`playback_cleared`", "`terminal_cancelled`"};
  for (const char* const phase : phases) {
    CHECK_MESSAGE(summary.find(phase) != std::string::npos,
                  std::string("摘要缺少取消阶段 ") + phase);
  }
  CHECK(summary.find("阶段缺失") == std::string::npos);
  // 冻结之后的迟到回调不得改写快照：run_end 必须是最后一条事件。
  const std::vector<std::string> events = Lines(result.events_jsonl);
  CHECK(!events.empty());
  CHECK(events.back().find("\"name\":\"run_end\"") != std::string::npos);
  std::filesystem::remove_all(dir);
}

// 保护不变量 5：未测量不等于 0。摘要为每个缺失的候选里程碑给出原因，指标流里则完全不出现。
void TestUnmeasuredMilestonesAreLabelledInsteadOfZero() {
  const std::string dir = EvidenceDir("unmeasured");
  std::filesystem::remove_all(dir);
  const MockProfileResult normal = run_mock_profile(ConfigFor(MockProfileScenario::kNormal, dir));
  std::string summary;
  CHECK(ReadFile(dir + "/summary.md", summary));

  // L1 直答没有经过生成后端，首 token 因此是"路径不同"，必须写明而不是留一个空洞。
  CHECK(!HasMetric(normal.metrics_jsonl, "mono_first_token_us"));
  CHECK(summary.find("`first_token`") != std::string::npos);
  CHECK(summary.find("未测量：") != std::string::npos);
  CHECK(summary.find("没有从生成后端收到 token") != std::string::npos);
  // 没有发生的取消不会产生任何取消指标。
  CHECK(!HasMetric(normal.metrics_jsonl, "mono_cancel_accepted_us"));
  CHECK(summary.find("本次运行没有发生取消") != std::string::npos);
  std::filesystem::remove_all(dir);
}

// 保护不变量 4：同一配置重复运行得到逐字节一致的里程碑序列与 step 指标。
// mono 族是实测值，因此只比较"可复现的部分"，并对实测部分只要求单调不减。
void TestRepeatedRunsReportTheSameSchedule() {
  // 两次运行写进同一个目录：执行命令里带着输出目录，用两个不同目录跑就不再是"同一份
  // 配置"了；而这里比较的是内存里的结果，与目录里的文件无关。
  const std::string dir = EvidenceDir("repeat");
  std::filesystem::remove_all(dir);
  const MockProfileResult first =
      run_mock_profile(ConfigFor(MockProfileScenario::kSlowConsumer, dir));
  const MockProfileResult second =
      run_mock_profile(ConfigFor(MockProfileScenario::kSlowConsumer, dir));

  CHECK(Names(first.milestones) == Names(second.milestones));
  for (std::size_t index = 0; index < first.milestones.size(); ++index) {
    CHECK(first.milestones[index].step == second.milestones[index].step);
    CHECK(first.milestones[index].source == second.milestones[index].source);
  }
  CHECK(StepMetrics(first.metrics_jsonl) == StepMetrics(second.metrics_jsonl));

  // 清单里的日历时间是实测值，因此规范化之后才比较；其余字段（含三个哈希与命令）必须一致。
  const auto strip = [](std::string manifest) {
    for (const char* const key : {"start_time", "end_time"}) {
      const std::string needle = std::string("\"") + key + "\":\"";
      const std::size_t begin = manifest.find(needle);
      if (begin == std::string::npos) {
        continue;
      }
      const std::size_t value_begin = begin + needle.size();
      const std::size_t value_end = manifest.find('"', value_begin);
      if (value_end != std::string::npos) {
        manifest.replace(value_begin, value_end - value_begin, "<time>");
      }
    }
    return manifest;
  };
  CHECK(strip(first.manifest_json) == strip(second.manifest_json));
  std::filesystem::remove_all(dir);
}

// 保护不变量 6：产物策略决定归宿。transient 之后目录为空（清理路径确实被执行过），
// retained 之后文件仍在且账目记为"有意保留"。两种归宿都不能被报告成"资源没交还"。
void TestArtifactPolicyDecidesWhereTheArtifactsEnd() {
  const std::string dir = EvidenceDir("policy");
  std::filesystem::remove_all(dir);

  MockProfileConfig transient = ConfigFor(MockProfileScenario::kNormal, dir);
  transient.artifact_policy = MockProfileArtifactPolicy::kTransient;
  const MockProfileResult removed = run_mock_profile(transient);
  CHECK(removed.ledger.artifacts_written == 5);
  CHECK(removed.ledger.artifacts_removed);
  CHECK(!removed.ledger.artifacts_retained);
  CHECK(removed.ledger.quiesced());
  CHECK(std::filesystem::is_empty(dir));

  MockProfileConfig retained = ConfigFor(MockProfileScenario::kNormal, dir);
  const MockProfileResult kept = run_mock_profile(retained);
  CHECK(kept.ledger.artifacts_written == 5);
  CHECK(!kept.ledger.artifacts_removed);
  CHECK(kept.ledger.artifacts_retained);
  CHECK(kept.ledger.quiesced());
  CHECK(std::filesystem::exists(dir + "/summary.md"));
  CHECK(std::filesystem::exists(dir + "/run-manifest.json"));

  std::filesystem::remove_all(dir);
}

// 保护不变量 6：要求保留却不给目录是一条配置错误，而不是一次静默的空保留——
// 调用方会以为证据已经落盘。
void TestRetainedPolicyWithoutDirectoryIsRejected() {
  MockProfileConfig config = ConfigFor(MockProfileScenario::kNormal, std::string());
  config.artifact_policy = MockProfileArtifactPolicy::kRetained;
  const MockProfileResult result = run_mock_profile(config);
  CHECK(!result.error.ok());
  CHECK(result.error.code == ErrorCode::kInvalidInput);
  CHECK(result.exit_code == 1);
  CHECK(result.ledger.connections_opened == 0);
  CHECK(result.ledger.threads_created == 0);
  CHECK(result.ledger.quiesced());
}

}  // namespace

int main() {
  TestEachScenarioProducesCompleteEvidence();
  TestOverlapIsProvenByStepOrderAndPositiveLead();
  TestCancellationKeepsOnlyTheAudioWrittenBeforeTheStop();
  TestUnmeasuredMilestonesAreLabelledInsteadOfZero();
  TestRepeatedRunsReportTheSameSchedule();
  TestArtifactPolicyDecidesWhereTheArtifactsEnd();
  TestRetainedPolicyWithoutDirectoryIsRejected();
  std::printf("mock_profile_evidence: 全部检查通过\n");
  return 0;
}

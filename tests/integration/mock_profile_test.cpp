// Mock profile 确定性入口的集成夹具：验收「一条命令跑一个可复现场景」这件事本身。
//
// 分工：会话编排由 session_*_test.cpp 验收，监督器生命周期由 supervisor_lifecycle_test.cpp
// 验收，请求入口由 gateway_entry_test.cpp 验收。本文件只回答 profile 自己的问题——场景能不能
// 显式选择、同一配置能不能重复跑出同样的结果、失败时退出码与清理是否都明确。
//
// 保护的不变量（每条在断言旁注明）：
//   1. 四个场景都能被显式选择，并且各自跑出预期的因果形态；场景标识可解析、可回显。
//   2. 确定性针对内容与因果顺序：同一配置重复执行得到逐字节一致的报文与汇总，且不含时间戳。
//   3. 返回即无残留：监督器工作线程已 join、连接已关闭、输出产物已删除，账目自证。
//   4. 失败路径与成功路径一样明确：故障场景给出失败终态与失败退出码，但清理照常完成。
//   5. 取消是正常语义：退出码仍为 0，而终态明确是取消；打断点由配置决定且在打断前确实出了声。
//   6. 配置错误在建立任何会话之前被拒绝，不产生连接、线程或文件。
//   7. 慢消费由有界发送缓冲触发关闭，而不是靠睡眠或运气；关闭之后会话仍然收敛干净。
//
// 关于音频交付的口径：v1 的请求入口只交付逐轮文本与终态，逐帧 PCM 下行属于后续传输任务。
// 因此"回答出了声"的证据取自会话自己的运行记录（rendered_frames），而不是线上事件数——
// 把后者当证据会让这条用例在一个正确的实现上永远失败。
#include "../test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "mock_profile.hpp"

using nexweave::app::MockProfileConfig;
using nexweave::app::MockProfileExpectation;
using nexweave::app::MockProfileResult;
using nexweave::app::MockProfileScenario;
using nexweave::app::mock_profile_expectation;
using nexweave::app::mock_profile_scenario_slug;
using nexweave::app::parse_mock_profile_scenario;
using nexweave::app::run_mock_profile;
using nexweave::app::to_string;
using nexweave::app::validate_mock_profile_config;
using nexweave::domain::ErrorCode;

namespace {

// 全部场景的枚举值。遍历它可以让"可解析、可回显、可运行、形态相符"四项检查在新增场景时
// 自动覆盖，而不是靠人记得补一条。
const MockProfileScenario kAllScenarios[] = {
    MockProfileScenario::kNormal, MockProfileScenario::kSlowConsumer,
    MockProfileScenario::kCancel, MockProfileScenario::kFault};

MockProfileConfig ConfigFor(MockProfileScenario scenario) {
  MockProfileConfig config;
  config.scenario = scenario;
  config.stream_id = std::string("mock-") + mock_profile_scenario_slug(scenario);
  if (scenario == MockProfileScenario::kCancel) {
    // 等播放组件写出第 1 帧之后再受理停止：它保证"取消发生在已经出声之后"，同时远早于
    // 会话自然收尾，因此"取消真的打断了在途工作"与"取消没有伪装成自然结束"可以同时验证。
    config.cancel_after_pcm_events = 1;
  }
  if (scenario == MockProfileScenario::kSlowConsumer) {
    // 每次只取走 8 字节：小于任何一条事件的编码长度，因此队列的增长由契约决定，而不是由
    // "这一轮恰好没读"这种时序巧合决定。
    config.drain_budget_bytes = 8;
  }
  return config;
}

// 只取出某一类记录（保留前缀之后的正文）。前缀是本层公共契约的一部分。
std::vector<std::string> RecordsWithPrefix(const MockProfileResult& result,
                                           const std::string& prefix) {
  std::vector<std::string> selected;
  for (const std::string& record : result.records) {
    if (record.compare(0, prefix.size(), prefix) == 0) {
      selected.push_back(record.substr(prefix.size()));
    }
  }
  return selected;
}

// 汇总里的扁平字段取值；够用且不引入解析依赖。找不到返回空串。
std::string SummaryField(const MockProfileResult& result, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t at = result.summary_json.find(needle);
  if (at == std::string::npos) {
    return std::string();
  }
  const std::size_t begin = at + needle.size();
  if (begin < result.summary_json.size() && result.summary_json[begin] == '"') {
    const std::size_t end = result.summary_json.find('"', begin + 1);
    return result.summary_json.substr(begin + 1, end - begin - 1);
  }
  std::size_t end = begin;
  while (end < result.summary_json.size() && result.summary_json[end] != ',' &&
         result.summary_json[end] != '}') {
    ++end;
  }
  return result.summary_json.substr(begin, end - begin);
}

// 把形态压成一行文本：预期与实测不一致时要能直接看出差在哪，而不是只看到一个布尔。
std::string ShapeText(const MockProfileExpectation& shape) {
  return std::string("accepted=") + (shape.session_accepted ? "1" : "0") +
         " completed=" + (shape.session_completed ? "1" : "0") +
         " failure=" + (shape.failure_event ? "1" : "0") +
         " cancelled=" + (shape.cancellation_observed ? "1" : "0") +
         " audio=" + (shape.audio_rendered ? "1" : "0") +
         " server_close=" + (shape.connection_closed_by_server ? "1" : "0") +
         " slow_close=" + (shape.slow_client_close ? "1" : "0") +
         " text=" + (shape.text_delivered ? "1" : "0");
}

bool SameShape(const MockProfileExpectation& left, const MockProfileExpectation& right) {
  return left.session_accepted == right.session_accepted &&
         left.session_completed == right.session_completed &&
         left.failure_event == right.failure_event &&
         left.cancellation_observed == right.cancellation_observed &&
         left.audio_rendered == right.audio_rendered &&
         left.connection_closed_by_server == right.connection_closed_by_server &&
         left.slow_client_close == right.slow_client_close &&
         left.text_delivered == right.text_delivered;
}

// 写一个最小的合法 WAV（16 kHz / 单声道 / 16 位 PCM，2 帧静音）。
//
// 由测试自己生成而不是提交一个二进制夹具：夹具进版本库会让"这个文件是什么"变成需要额外
// 解释的东西，而 44 字节头 + 数据的构造方式本身就是被测代码所接受的格式说明。
bool WriteSilentWav(const std::string& path) {
  const std::uint32_t data_bytes = 2 * 320 * 2;
  std::vector<std::uint8_t> bytes;
  const auto put16 = [&bytes](std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
  };
  const auto put32 = [&bytes](std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  };
  const auto put_tag = [&bytes](const char* tag) {
    for (int index = 0; index < 4; ++index) {
      bytes.push_back(static_cast<std::uint8_t>(tag[index]));
    }
  };
  put_tag("RIFF");
  put32(36 + data_bytes);
  put_tag("WAVE");
  put_tag("fmt ");
  put32(16);
  put16(1);       // PCM
  put16(1);       // 单声道
  put32(16000);   // 采样率
  put32(16000 * 2);
  put16(2);       // 块对齐
  put16(16);      // 位深
  put_tag("data");
  put32(data_bytes);
  bytes.insert(bytes.end(), data_bytes, 0);
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
  const int closed = std::fclose(file);
  return written == bytes.size() && closed == 0;
}

// ---- 用例 ----

// 保护不变量 1：场景标识是稳定契约。四个场景都必须能按名字叫出来、能解析回来、能给出非空
// 的标识前缀；非法名字必须被拒绝，而不是悄悄回退到某个默认场景。
void TestScenarioNamesRoundTrip() {
  for (const MockProfileScenario scenario : kAllScenarios) {
    const char* name = to_string(scenario);
    CHECK(name != nullptr);
    const std::string text(name);
    CHECK(!text.empty());
    CHECK(!std::string(mock_profile_scenario_slug(scenario)).empty());
    MockProfileScenario parsed = MockProfileScenario::kNormal;
    CHECK(parse_mock_profile_scenario(text, parsed));
    CHECK(parsed == scenario);
  }
  MockProfileScenario untouched = MockProfileScenario::kCancel;
  CHECK(!parse_mock_profile_scenario("bogus", untouched));
  CHECK(untouched == MockProfileScenario::kCancel);
  CHECK(!parse_mock_profile_scenario("", untouched));
}

// 保护不变量 1 与 4：每个场景的预期形态互不相同，且实测形态与预期逐项一致。这条用例是
// "场景确实被区分开"的直接证据——四个场景若跑出同一个形态，它必然失败。
void TestEachScenarioMatchesItsExpectedShape() {
  for (const MockProfileScenario scenario : kAllScenarios) {
    const MockProfileResult result = run_mock_profile(ConfigFor(scenario));
    const MockProfileExpectation expected = mock_profile_expectation(scenario);
    const std::string detail = std::string("场景 ") + to_string(scenario) + " 形态不符：预期[" +
                               ShapeText(expected) + "] 实测[" + ShapeText(result.observed) +
                               "] 首处不符=" + result.mismatch + " 汇总=" + result.summary_json;
    CHECK_MESSAGE(SameShape(result.observed, expected), detail);
    CHECK(result.expectation_matched);
    CHECK(result.mismatch.empty());
    CHECK(result.error.ok());
    CHECK(result.exit_code == 0);
    CHECK(result.ledger.quiesced());
    CHECK(result.scenario_name == to_string(scenario));
  }
}

// 保护不变量 2：确定性。同一配置两次运行的报文、汇总与清单必须逐字节一致，且输出里不得
// 出现时间戳——否则"确定性"只是碰巧两次跑得一样快。
void TestRepeatedRunsAreByteIdentical() {
  for (const MockProfileScenario scenario : kAllScenarios) {
    const MockProfileResult first = run_mock_profile(ConfigFor(scenario));
    const MockProfileResult second = run_mock_profile(ConfigFor(scenario));
    CHECK(first.exit_code == second.exit_code);
    CHECK(first.records == second.records);
    CHECK(first.summary_json == second.summary_json);
    CHECK(first.manifest_json == second.manifest_json);
    CHECK(!first.records.empty());
    CHECK(first.summary_json.find("timestamp") == std::string::npos);
    CHECK(first.summary_json.find("time_ms") == std::string::npos);
    // 汇总里没有 run_id：清单才是承载运行身份的记录，汇总只带场景与配置指纹。
    CHECK(SummaryField(first, "run_id").empty());
    CHECK(first.manifest_json.find("mock-" + std::string(to_string(scenario))) != std::string::npos);
    CHECK(first.manifest_json.find("\"profile\":\"mock\"") != std::string::npos);
  }
}

// 保护不变量 1 与 3：正常场景的交付顺序与内容。一轮 L1 直答应当给出文本、把音频交给播放，
// 并以 done 收尾；终态必须是最后一条事件，否则客户端会把一个中间状态当成收尾。
void TestNormalDeliversTextAudioAndDone() {
  const MockProfileResult normal = run_mock_profile(ConfigFor(MockProfileScenario::kNormal));
  CHECK(normal.response_count == 1);
  CHECK(normal.token_event_count == 1);
  CHECK(normal.rendered_frames >= 1);
  CHECK(normal.observed.audio_rendered);
  CHECK(normal.terminal_event_count == 1);
  CHECK(normal.terminal_error.ok());
  const std::vector<std::string> events = RecordsWithPrefix(normal, "event ");
  CHECK(!events.empty());
  CHECK(events.back().find("\"end\":true") != std::string::npos);
  CHECK(events.back().find("\"type\":\"done\"") != std::string::npos);
}

// 保护不变量 5：取消是正常语义。退出码仍为 0，而终态明确是取消；因为打断点设在"第一帧已经
// 写出之后"，音频确实产生过，"取消不删除已经播出的声音"这条约定同时得到保护。
void TestCancelStopsAfterFirstFrameAndReportsCancellation() {
  const MockProfileResult cancelled = run_mock_profile(ConfigFor(MockProfileScenario::kCancel));
  CHECK(cancelled.exit_code == 0);
  CHECK(cancelled.error.ok());
  CHECK(cancelled.observed.cancellation_observed);
  CHECK(cancelled.terminal_error.code == ErrorCode::kCancelled);
  CHECK(cancelled.rendered_frames >= 1);
  CHECK(cancelled.observed.audio_rendered);
  // 受理停止时确实已经有帧写出：这是"取消发生在出声之后"的可核对形式。
  CHECK(cancelled.cancel_frames >= 1);
  CHECK(!cancelled.observed.session_completed);
  CHECK(cancelled.observed.failure_event);
  CHECK(cancelled.ledger.quiesced());
}

// 保护不变量 7：慢消费必须由有界缓冲触发关闭，而不是靠等待或运气。这条用例同时证明
// "缓冲不会随输出无界增长"与"连接被关闭不等于会话没有收敛"。
void TestSlowConsumerClosesBoundedBufferAndStillQuiesces() {
  const MockProfileResult slow = run_mock_profile(ConfigFor(MockProfileScenario::kSlowConsumer));
  CHECK(slow.exit_code == 0);
  CHECK(slow.error.ok());
  CHECK(slow.observed.connection_closed_by_server);
  CHECK(slow.observed.slow_client_close);
  CHECK(slow.close_reason == "slow_client");
  CHECK(slow.ledger.connections_open == 0);
  CHECK(slow.ledger.quiesced());
  // 服务端关闭表达的是"发不完"，不是"识别失败"：会话本身仍然被受理并产生了回答。
  CHECK(slow.observed.session_accepted);
  CHECK(slow.rendered_frames >= 1);
}

// 保护不变量 4：故障路径。输入在开始任何轮次之前不可用，因此会话以明确错误收敛；退出码为
// 失败，但清理照常完成，账目仍然自证。
void TestFaultFailsWithExplicitErrorAndStillCleansUp() {
  const MockProfileResult fault = run_mock_profile(ConfigFor(MockProfileScenario::kFault));
  // 退出码为 0 说的是"这条命令按预期完成了一次故障验收"，不是"会话失败了"。会话确实失败，
  // 由失败终态与错误码回答——把两者混成一个数字，会让"预期的失败"和"没跑起来"无法区分。
  CHECK(fault.exit_code == 0);
  CHECK(fault.observed.session_accepted);
  CHECK(fault.observed.failure_event);
  CHECK(!fault.observed.session_completed);
  CHECK(!fault.observed.audio_rendered);
  CHECK(fault.rendered_frames == 0);
  CHECK(fault.terminal_event_count == 1);
  CHECK(!fault.terminal_error.ok());
  CHECK(fault.terminal_error.code == ErrorCode::kInvalidInput);
  // 失败路径的清理与成功路径同一套：工作线程已 join、连接已关闭、没有产物残留。
  CHECK(fault.ledger.quiesced());

  // 退出码 1 留给"运行本身没有按契约完成"，其中一类是**场景形态不符**：命令跑完了，但跑出
  // 来的形态不是这个场景声明的形态，于是这份证据不能用来支持该场景的结论。
  //
  // 构造方式：故障场景声明"输入不可用"，这里改写输入路径让它指向一个真的存在的合法 WAV，
  // 于是会话会正常完成——`session_completed` 与预期相反，运行必须以失败收尾。
  // 夹具写在系统临时目录：构建目录不一定存在名为 build 的子目录（out-of-tree 构建会把它
  // 放在别处），因此不能假设任何相对路径可写。文件名带上进程号，避免并行运行的两个测试
  // 进程互相覆盖对方的夹具。
  const std::string fixture_path =
      (std::filesystem::temp_directory_path() /
       ("nexweave-fault-fixture-" + std::to_string(::getpid()) + ".wav"))
          .string();
  if (!WriteSilentWav(fixture_path)) {
    CHECK_MESSAGE(false, "无法写入测试用 WAV 夹具，形态不符用例无法执行");
  } else {
    MockProfileConfig wrong_input = ConfigFor(MockProfileScenario::kFault);
    wrong_input.wav_path = fixture_path;
    const MockProfileResult mismatched = run_mock_profile(wrong_input);
    CHECK(!mismatched.expectation_matched);
    CHECK(!mismatched.mismatch.empty());
    CHECK(mismatched.exit_code == 1);
    // 形态不符不改变清理：资源账目仍然自证，失败不会被当成"没跑起来"。
    CHECK(mismatched.ledger.quiesced());
    std::remove(fixture_path.c_str());
  }
}

// 保护不变量 6：配置错误在建立任何会话之前被拒绝。没有连接、没有线程、没有文件，也没有任何
// "跑了一半"的场景状态。
void TestInvalidConfigIsRejectedBeforeAnySession() {
  MockProfileConfig empty_stream;
  empty_stream.stream_id.clear();
  const MockProfileResult rejected = run_mock_profile(empty_stream);
  CHECK(!rejected.error.ok());
  CHECK(rejected.error.code == ErrorCode::kInvalidInput);
  CHECK(rejected.exit_code == 1);
  CHECK(rejected.ledger.connections_opened == 0);
  CHECK(rejected.ledger.threads_created == 0);
  CHECK(rejected.records.empty());
  CHECK(rejected.ledger.quiesced());

  // 旋钮与场景必须自洽：把取消旋钮写在正常场景上，说明"这条命令到底在验什么"已经不可解释，
  // 因此按配置错误拒绝，而不是默默忽略它。
  MockProfileConfig cancel_knob_mismatch;
  cancel_knob_mismatch.scenario = MockProfileScenario::kNormal;
  cancel_knob_mismatch.cancel_after_pcm_events = 3;
  CHECK(!validate_mock_profile_config(cancel_knob_mismatch).ok());

  MockProfileConfig drain_mismatch;
  drain_mismatch.scenario = MockProfileScenario::kCancel;
  drain_mismatch.drain_budget_bytes = 8;
  CHECK(!validate_mock_profile_config(drain_mismatch).ok());

  MockProfileConfig missing_drain;
  missing_drain.scenario = MockProfileScenario::kSlowConsumer;
  CHECK(!validate_mock_profile_config(missing_drain).ok());

  // 取消场景不接受 0 打断点：不设等待就等于让取消请求与会话收尾抢时序，同一配置时而生效
  // 时而失效。它必须是一条配置错误，而不是一个"有时候能用"的开关。
  MockProfileConfig no_interrupt_point;
  no_interrupt_point.scenario = MockProfileScenario::kCancel;
  no_interrupt_point.cancel_after_pcm_events = 0;
  CHECK(!validate_mock_profile_config(no_interrupt_point).ok());
  MockProfileConfig zero_interrupt;
  zero_interrupt.scenario = MockProfileScenario::kCancel;
  const MockProfileResult zero_result = run_mock_profile(zero_interrupt);
  CHECK(zero_result.exit_code == 1);
  CHECK(zero_result.error.code == ErrorCode::kInvalidInput);
  CHECK(zero_result.ledger.connections_opened == 0);

  MockProfileConfig unknown_scenario;
  unknown_scenario.scenario = static_cast<MockProfileScenario>(9);
  CHECK(!validate_mock_profile_config(unknown_scenario).ok());
}

// 保护不变量 5 与 2：打断点由配置决定，并且真的被执行。受理停止那一刻"已经写出多少帧"
// 必须随旋钮增大，这说明闸门真的把会话停在配置指定的位置，而不是被忽略或晚装。
void TestCancelKnobMovesTheInterruptionPoint() {
  MockProfileConfig early = ConfigFor(MockProfileScenario::kCancel);
  early.cancel_after_pcm_events = 1;
  const MockProfileResult early_result = run_mock_profile(early);

  MockProfileConfig later = ConfigFor(MockProfileScenario::kCancel);
  later.cancel_after_pcm_events = 3;
  const MockProfileResult later_result = run_mock_profile(later);

  CHECK(early_result.observed.cancellation_observed);
  CHECK(later_result.observed.cancellation_observed);
  CHECK(early_result.cancel_frames == 1);
  CHECK(later_result.cancel_frames == 3);
  CHECK(later_result.cancel_frames > early_result.cancel_frames);
  // 两次运行的配置指纹不同：旋钮参与配置标识，因此证据能区分"停在第几帧"。
  CHECK(SummaryField(early_result, "config_hash") != SummaryField(later_result, "config_hash"));
  CHECK(early_result.summary_json != later_result.summary_json);
}

// 保护不变量 3：输出目录里的产物在返回之前必须被清掉，账目如实报告写了几个文件。
void TestOutputArtifactsAreWrittenAndRemoved() {
  MockProfileConfig config = ConfigFor(MockProfileScenario::kNormal);
  config.output_dir = "build/mock-profile-artifacts-test";
  const MockProfileResult result = run_mock_profile(config);
  CHECK(result.ledger.artifacts_written == 3);
  CHECK(result.ledger.artifacts_removed);
  CHECK(result.ledger.quiesced());
  CHECK(result.exit_code == 0);
  CHECK(SummaryField(result, "output_configured") == "true");
}

// 保护不变量 3：连续跑不同场景不会互相污染。若某个场景留下了工作线程或连接，它会在下一条
// 命令里以"形态不符"或"账目不平"暴露出来，因此这里连跑全部场景并逐步核对。
void TestBackToBackScenariosDoNotInterfere() {
  for (const MockProfileScenario scenario : kAllScenarios) {
    const MockProfileResult result = run_mock_profile(ConfigFor(scenario));
    CHECK(result.ledger.quiesced());
    CHECK(result.ledger.threads_created <= 1);
    CHECK(result.ledger.connections_opened == 1);
    CHECK(result.scenario == scenario);
    CHECK(SummaryField(result, "scenario") == to_string(scenario));
    CHECK(SummaryField(result, "quiesced") == "true");
    CHECK(SummaryField(result, "config_hash").find("fnv1a64:") == 0);
    CHECK(SummaryField(result, "input_hash").find("fnv1a64:") == 0);
  }
}

}  // namespace

int main() {
  TestScenarioNamesRoundTrip();
  TestEachScenarioMatchesItsExpectedShape();
  TestRepeatedRunsAreByteIdentical();
  TestNormalDeliversTextAudioAndDone();
  TestCancelStopsAfterFirstFrameAndReportsCancellation();
  TestSlowConsumerClosesBoundedBufferAndStillQuiesces();
  TestFaultFailsWithExplicitErrorAndStillCleansUp();
  TestInvalidConfigIsRejectedBeforeAnySession();
  TestCancelKnobMovesTheInterruptionPoint();
  TestOutputArtifactsAreWrittenAndRemoved();
  TestBackToBackScenariosDoNotInterfere();
  std::printf("mock_profile: 全部检查通过\n");
  return 0;
}
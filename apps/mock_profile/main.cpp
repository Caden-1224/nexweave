// Mock profile 命令行入口：一条命令跑一个可复现场景。
//
// 职责：把命令行参数变成一次场景运行，并把结果按固定形状写到 stdout——场景横幅、逐行报文
// （response/event/metric 三种前缀）、以及最后一行机器可读汇总。它不实现场景逻辑、不组装能力、
// 不做清理判定：这些都在 core/app/mock_profile.* 里，命令行只负责参数与呈现。
//
// 为什么输出到 stdout 而不是只写文件：门禁与演示都需要“跑完就能看到结果”。文件是给需要
// 留档的调用方准备的，并且分两种用途：--out-dir 写完即删（验证清理路径确实被执行），
// --evidence-dir 写完保留（验证证据确实可留档）。
//
// 退出码：0 表示本次运行按契约收敛（含取消，以及故障场景按预期失败）；1 表示配置错误、形态
// 不符、产物未能清理或资源未交还。区分标准是“这一次运行是否按契约完成”，不是“会话是否成功
// 回答”——故障场景的预期就是失败，它按契约完成时退出码同样是 0。
//
// 资源归属：本进程只创建输出目录（若指定）与其中的临时产物；场景内部创建的监督器线程、连接
// 与文件都在 core 层返回前释放，并由汇总里的 quiesced 字段自证。本入口不后台启动任何进程。
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "mock_profile.hpp"

namespace {

using nexweave::app::MockProfileArtifactPolicy;
using nexweave::app::MockProfileConfig;
using nexweave::app::MockProfileResult;
using nexweave::app::MockProfileScenario;
using nexweave::app::parse_mock_profile_scenario;
using nexweave::app::run_mock_profile;
using nexweave::app::to_string;
using nexweave::app::validate_mock_profile_config;

void PrintUsage() {
  std::cout
      << "用法: nexweave_mock_profile --scenario <normal|slow|cancel|fault> [选项]\n"
         "\n"
         "场景（互斥，必须选一个）：\n"
         "  normal   一次创建被受理，会话跑到成功收敛，事件按发生顺序交付\n"
         "  slow     客户端取字节跟不上产出，入口的有界缓冲达到上限后关闭连接\n"
         "  cancel   会话出声之后受理停止；取消是正常语义，退出码仍为 0\n"
         "  fault    输入在开始任何轮次之前不可用；以明确错误收敛并以失败退出码收尾\n"
         "\n"
         "选项：\n"
         "  --stream-id <id>        会话与输入流标识（默认按场景派生）\n"
         "  --out-dir <dir>         把本次运行的五份产物写到该目录，运行结束前删除：\n"
         "                          它验证的是“返回后没有本进程残留”；目录不存在时创建\n"
         "  --evidence-dir <dir>    写同样的五份产物但保留下来作为运行证据：\n"
         "                          run-manifest.json、events.jsonl、metrics.jsonl、\n"
         "                          protocol.jsonl、summary.md\n"
         "  --cancel-after-pcm <n>  取消场景：等播放写出第 n 帧之后受理停止；\n"
         "                          n 必须大于 0（默认 1），要表达“用户立刻喊停”用 1\n"
         "  --drain-budget <n>      慢消费场景：每次最多取走的字节数（默认 8，必须为正）\n"
         "  --quiet                 不打印横幅与逐行报文，只输出汇总行\n"
         "  --events                额外输出逐行报文（数据面事件流）\n"
         "  --help                  打印本说明\n"
         "\n"
         "输出：默认只有第一行场景横幅与最后一行的机器可读汇总 JSON；\n"
         "      逐行报文（response/event 前缀）用 --events 打开，横幅用 --quiet 关闭。\n"
         "      同一命令重复运行输出逐字节一致（不含时间戳）。\n"
         "退出码：按契约完成为 0；配置错误、形态不符或资源未交还为 1。\n";
}

// 参数解析的三态结果。与单进程应用入口同一种形状：打印帮助与参数错误是两件事，
// 用两个布尔表示会出现一种没有意义的组合。
enum class ParseOutcome { kContinue, kHelpPrinted, kInvalid };

bool ParseSize(const std::string& text, std::size_t& value) {
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

struct Options {
  std::string scenario;
  std::string stream_id;
  std::string out_dir;
  // 产物在运行结束前是否删除。--out-dir 与 --evidence-dir 写的是同样的产物，区别只在
  // 这一位；两者同时出现会让“这次运行到底想验证清理还是想留档”不可解释，因此按参数
  // 错误拒绝，而不是让后写的那个悄悄覆盖前一个。
  bool retain = false;
  bool dir_given = false;
  // 旋钮的"没写"与"写了 0"必须分开：取消旋钮的 0 是有意义的值（不等输出就喊停），
  // 慢消费的 0 则是非法值。因此这里用一个超出合法范围的哨兵表示"调用方没有指定"，
  // 真正的默认值按场景给出——场景不同，合适的默认值本来也不同。
  static constexpr std::size_t kUnset = static_cast<std::size_t>(-1);
  std::size_t cancel_after_pcm = kUnset;
  std::size_t drain_budget = kUnset;
  bool quiet = false;
  bool events = false;
  bool stream_id_given = false;
};

ParseOutcome ParseOptions(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    const auto value = [index, argc, argv]() -> std::string {
      return index + 1 < argc ? std::string(argv[index + 1]) : std::string();
    };
    const auto take = [&index]() {
      ++index;
    };
    bool size_ok = true;
    const auto size_option = [&](std::size_t& target) {
      if (!ParseSize(value(), target)) {
        std::cerr << flag << " 需要非负整数" << std::endl;
        size_ok = false;
        return;
      }
      take();
    };

    if (flag == "--help" || flag == "-h") {
      PrintUsage();
      return ParseOutcome::kHelpPrinted;
    } else if (flag == "--quiet") {
      options.quiet = true;
    } else if (flag == "--events") {
      options.events = true;
    } else if (flag == "--scenario") {
      options.scenario = value();
      take();
    } else if (flag == "--out-dir" || flag == "--evidence-dir") {
      if (options.dir_given) {
        std::cerr << "--out-dir 与 --evidence-dir 只能给一个" << std::endl;
        return ParseOutcome::kInvalid;
      }
      options.out_dir = value();
      options.retain = flag == "--evidence-dir";
      options.dir_given = true;
      take();
    } else if (flag == "--stream-id") {
      options.stream_id = value();
      options.stream_id_given = true;
      take();
    } else if (flag == "--cancel-after-pcm") {
      size_option(options.cancel_after_pcm);
    } else if (flag == "--drain-budget") {
      size_option(options.drain_budget);
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
  switch (ParseOptions(argc, argv, options)) {
    case ParseOutcome::kHelpPrinted:
      return 0;  // 打印帮助是正常结束，不是参数错误。
    case ParseOutcome::kInvalid:
      return 1;
    case ParseOutcome::kContinue:
      break;
  }

  MockProfileConfig config;
  if (!parse_mock_profile_scenario(options.scenario, config.scenario)) {
    std::cerr << "--scenario 必须是 normal、slow、cancel 或 fault" << std::endl;
    return 1;
  }
  // 旋钮只在对应场景下生效：把默认值写进其他场景会被配置校验拒绝，因此这里按场景决定是否
  // 携带它们，使"命令行没写"与"写了但不适用"得到同样的结果，而不是两种不同的失败。
  if (config.scenario == MockProfileScenario::kCancel) {
    // 默认停在第 1 帧之后：既有音频已经出声的证据，又远早于会话自然收尾。
    config.cancel_after_pcm_events =
        options.cancel_after_pcm == Options::kUnset ? 1 : options.cancel_after_pcm;
  } else if (options.cancel_after_pcm != Options::kUnset) {
    std::cerr << "--cancel-after-pcm 只在取消场景下有意义" << std::endl;
    return 1;
  }
  if (config.scenario == MockProfileScenario::kSlowConsumer) {
    // 默认每次取 8 字节：小于任何一条报文的编码长度，因此队列的增长由契约决定。
    config.drain_budget_bytes = options.drain_budget == Options::kUnset ? 8 : options.drain_budget;
  } else if (options.drain_budget != Options::kUnset) {
    std::cerr << "--drain-budget 只在慢消费场景下有意义" << std::endl;
    return 1;
  }
  config.output_dir = options.out_dir;
  if (options.retain) {
    config.artifact_policy = MockProfileArtifactPolicy::kRetained;
  }
  if (options.stream_id_given) {
    config.stream_id = options.stream_id;
  } else {
    config.stream_id = std::string("mock-") + to_string(config.scenario);
  }

  const auto valid = validate_mock_profile_config(config);
  if (!valid.ok()) {
    std::cerr << "配置错误: " << valid.error.message << std::endl;
    return 1;
  }

  const MockProfileResult result = run_mock_profile(config);

  // 输出形状固定：横幅（可用 --quiet 关闭）→ 逐行报文（可用 --events 打开）→ 汇总行。
  // 汇总行必须在最后，且是唯一以 { 开头独占一行的内容，这样调用方可以直接取最后一行解析。
  if (!options.quiet) {
    std::cout << "mock_profile: scenario=" << result.scenario_name
              << " stream=" << config.stream_id
              << " exit_code=" << result.exit_code << std::endl;
    if (config.artifact_policy == MockProfileArtifactPolicy::kRetained) {
      // 留档运行要能一眼看出证据写到了哪：这条命令的价值就是“跑完就有可读的证据”，
      // 让调用方自己去猜目录不算交付。
      std::cout << "evidence_dir: " << config.output_dir << std::endl;
    }
  }
  if (options.events) {
    for (const std::string& record : result.records) {
      std::cout << record << std::endl;
    }
  }
  if (!result.error.ok()) {
    // 错误码按数值打印：领域层只导出错误码本身，不给它加流操作符；汇总行里有它的稳定
    // 文本名，因此这里只需要一个能对上汇总的诊断。
    std::cerr << "运行失败: code=" << static_cast<int>(result.error.code) << " "
              << result.error.message << std::endl;
  }
  if (!result.expectation_matched) {
    std::cerr << "场景形态不符: " << result.mismatch << std::endl;
  }
  std::cout << result.summary_json << std::endl;
  return result.exit_code;
}
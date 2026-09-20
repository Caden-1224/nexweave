// Linux profile 单进程/多进程对照测试。
//
// 这批用例共同保护：
//   - 两条执行拓扑使用同一份 Fake 后端脚本、同一输入帧序、同一队列容量与播放策略；
//   - 主动改变的只有执行拓扑：同一进程内直接运行 SessionApp，或由 LinuxProfile 驱动独立
//     Session 子进程；
//   - 两种拓扑都必须成功收敛，并产生相同的最终文本与 PCM 帧数；
//   - 原始样本包含会话延迟、父/子进程 RSS 峰值、队列峰值和成功率，汇总只做统计，不把
//     Fake 调度耗时写成模型、NPU 或板端性能。
#include "../support/fake_session_harness.hpp"
#include "../test_support.hpp"
#include "linux_profile.hpp"
#include "nexweave_version_from_cmake.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef NEXWEAVE_REMOTE_SESSION_FIXTURE
#error "测试需要 NEXWEAVE_REMOTE_SESSION_FIXTURE 指向远端 Session 夹具"
#endif

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using nexweave::app::LinuxProfile;
using nexweave::app::LinuxProfileConfig;
using nexweave::domain::AudioFrame;
using nexweave::protocol::DataEvent;
using nexweave::protocol::DataEventType;

constexpr std::size_t kDefaultWarmupRounds = 2;
constexpr std::size_t kDefaultMeasuredRounds = 6;
constexpr std::size_t kMaxRounds = 1000;
constexpr std::size_t kFrameSamples = nexweave::domain::kAudioFrameSamples;
constexpr std::size_t kFrameBytes = nexweave::domain::kAudioFrameBytes;

// 同一组确定性输入帧：两帧人声后两帧静音，恰好触发一个完整轮次。两条拓扑必须逐帧使用
// 同一序列，否则比较的就成了输入差异而不是拓扑差异。
const std::vector<std::int16_t> kInputValues = {1000, 1000, 0, 0};

struct Sample {
  std::size_t round = 0;
  bool success = false;
  std::string error;
  // 从开始提交输入到收到终态的墙钟耗时。它排除多进程子进程启动，只比较数据面与会话执行；
  // 子进程启动单独记录为 node_start_latency_us。
  std::uint64_t session_latency_us = 0;
  // 多进程路径从 profile.start() 到子进程就绪的耗时；单进程无独立节点，保持 0 并单独说明。
  std::uint64_t node_start_latency_us = 0;
  std::uint64_t parent_rss_peak_kb = 0;
  std::uint64_t child_rss_peak_kb = 0;
  bool child_rss_measured = false;
  std::size_t input_queue_peak = 0;
  std::size_t output_queue_peak = 0;
  std::size_t pcm_frames = 0;
  std::string text;
};

std::uint64_t ElapsedUs(Clock::time_point begin, Clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
}

AudioFrame MakeFrame(std::int16_t value) {
  const auto frame = AudioFrame::from_samples(
      std::vector<std::int16_t>(kFrameSamples, value));
  CHECK(frame.ok());
  return frame.value;
}

std::optional<std::uint64_t> ReadVmRssKb(pid_t pid) {
  std::ifstream file("/proc/" + std::to_string(pid) + "/status");
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::string line;
  while (std::getline(file, line)) {
    if (line.rfind("VmRSS:", 0) != 0) {
      continue;
    }
    std::istringstream stream(line);
    std::string key;
    std::string unit;
    std::uint64_t value = 0;
    if (stream >> key >> value >> unit) {
      return value;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<pid_t> FindProcessPid(const std::string& executable) {
  std::error_code error;
  std::filesystem::directory_iterator iterator("/proc", error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    const std::string name = iterator->path().filename().string();
    bool numeric = !name.empty();
    for (const char digit : name) {
      if (digit < '0' || digit > '9') {
        numeric = false;
        break;
      }
    }
    if (numeric) {
      std::ifstream cmdline(iterator->path() / "cmdline", std::ios::binary);
      std::string argument;
      if (cmdline.is_open() && std::getline(cmdline, argument, '\0') &&
          argument == executable) {
        try {
          return static_cast<pid_t>(std::stoul(name));
        } catch (...) {
          return std::nullopt;
        }
      }
    }
    iterator.increment(error);
  }
  return std::nullopt;
}

class RssSampler {
 public:
  void SampleParent() {
    const auto rss = ReadVmRssKb(::getpid());
    if (rss.has_value() && *rss > parent_peak_kb_) {
      parent_peak_kb_ = *rss;
    }
  }

  void SampleChild(pid_t pid) {
    if (pid <= 0) {
      return;
    }
    const auto rss = ReadVmRssKb(pid);
    if (!rss.has_value()) {
      return;
    }
    child_measured_ = true;
    if (*rss > child_peak_kb_) {
      child_peak_kb_ = *rss;
    }
  }

  std::uint64_t parent_peak_kb() const noexcept {
    return parent_peak_kb_;
  }

  std::uint64_t child_peak_kb() const noexcept {
    return child_peak_kb_;
  }

  bool child_measured() const noexcept {
    return child_measured_;
  }

 private:
  std::uint64_t parent_peak_kb_ = 0;
  std::uint64_t child_peak_kb_ = 0;
  bool child_measured_ = false;
};

Sample RunSingleProcess(std::size_t round) {
  Sample sample;
  sample.round = round;

  // 组装本身不计入会话延迟：两条拓扑都把“节点/夹具已经准备好”作为会话计时起点。
  nexweave::test::FakeSessionHarness harness;

  auto future = std::async(std::launch::async, [&harness] {
    return harness.app().run();
  });
  RssSampler sampler;
  const auto session_begin = Clock::now();

  bool pushed = true;
  std::string error;
  for (const std::int16_t value : kInputValues) {
    const auto queued = harness.source().push(MakeFrame(value));
    if (!queued.ok()) {
      pushed = false;
      error = queued.error.message;
      break;
    }
  }
  const auto finished_input = harness.source().end_input();
  if (!finished_input.ok()) {
    pushed = false;
    error = finished_input.error.message;
  }

  while (future.wait_for(0ms) != std::future_status::ready) {
    sampler.SampleParent();
    std::this_thread::sleep_for(1ms);
  }
  const nexweave::runtime::SessionAppRunResult result = future.get();
  const auto session_end = Clock::now();
  sampler.SampleParent();

  sample.session_latency_us = ElapsedUs(session_begin, session_end);
  sample.parent_rss_peak_kb = sampler.parent_peak_kb();
  sample.input_queue_peak = harness.source().stats().peak_pending_frames;

  const bool converged = result.error.ok() && result.cleanup_error.ok() &&
                         result.turns_completed == 1 && result.turns.size() == 1 &&
                         result.turns[0].completed && !result.turns[0].text.empty() &&
                         !result.turns[0].pcm_frames.empty();
  sample.success = pushed && converged;
  if (result.turns.size() == 1) {
    sample.text = result.turns[0].text;
    sample.pcm_frames = result.turns[0].pcm_frames.size();
    sample.output_queue_peak = result.turns[0].peak_pending_frames;
  }
  if (!sample.success) {
    if (!error.empty()) {
      sample.error = error;
    } else if (!result.error.ok()) {
      sample.error = result.error.message;
    } else if (!result.cleanup_error.ok()) {
      sample.error = result.cleanup_error.message;
    } else {
      sample.error = "单进程轮次没有形成完整终态";
    }
  }
  return sample;
}

LinuxProfileConfig MakeMultiProcessConfig(std::size_t round) {
  LinuxProfileConfig config;
  config.proxy.child.executable = NEXWEAVE_REMOTE_SESSION_FIXTURE;
  config.proxy.child.expect_ready_signal = true;
  config.proxy.child_config.start_wait_budget = 5000ms;
  config.proxy.child_config.stop_wait_budget = 500ms;
  config.proxy.child_config.kill_wait_budget = 1000ms;
  config.proxy.child_config.poll_interval = 2ms;
  config.proxy.endpoint_file =
      "/tmp/nexweave-compare-" + std::to_string(::getpid()) + "-" +
      std::to_string(round);
  config.proxy.ready_timeout = 5000ms;
  config.proxy.pump_interval = 1ms;
  config.proxy.max_pending_input_events = 64;
  config.proxy.max_pending_output_events = 256;
  config.proxy.terminal_cache_capacity = 8;
  config.proxy.terminal_retention = 30000ms;
  config.stream_id = "compare-" + std::to_string(round);
  config.first_generation = 1;
  return config;
}

Sample RunMultiProcess(std::size_t round) {
  Sample sample;
  sample.round = round;

  LinuxProfileConfig config = MakeMultiProcessConfig(round);
  const std::string endpoint_file = config.proxy.endpoint_file;
  LinuxProfile profile(std::move(config));

  const auto startup_begin = Clock::now();
  const auto started = profile.start();
  const auto startup_end = Clock::now();
  sample.node_start_latency_us = ElapsedUs(startup_begin, startup_end);
  if (!started.ok()) {
    sample.error = started.error.message;
    std::error_code ignored;
    std::filesystem::remove(endpoint_file, ignored);
    return sample;
  }

  const std::optional<pid_t> child_pid =
      FindProcessPid(NEXWEAVE_REMOTE_SESSION_FIXTURE);
  RssSampler sampler;
  const auto session_begin = Clock::now();
  bool stream_started = false;
  bool input_finished = false;
  bool terminal_done = false;
  std::string error;
  std::size_t pcm_frames = 0;
  std::string text;

  const auto stream = profile.start_stream();
  if (stream.ok()) {
    stream_started = true;
  } else {
    error = stream.error.message;
  }

  bool pushed = stream_started;
  if (stream_started) {
    for (const std::int16_t value : kInputValues) {
      const auto queued = profile.push_frame(MakeFrame(value));
      if (!queued.ok()) {
        pushed = false;
        error = queued.error.message;
        break;
      }
    }
  }
  if (pushed) {
    const auto finished = profile.finish_stream();
    if (finished.ok()) {
      input_finished = true;
    } else {
      error = finished.error.message;
    }
  }

  const auto deadline = Clock::now() + 5s;
  std::vector<nexweave::protocol::DataEvent> events;
  while (Clock::now() < deadline && !terminal_done) {
    events.clear();
    (void)profile.poll_events(events, 32, 1ms);
    for (const nexweave::protocol::DataEvent& event : events) {
      if (event.type == DataEventType::kFinal && !event.text.empty()) {
        text += event.text;
      }
      if (event.type == DataEventType::kPcm) {
        ++pcm_frames;
      }
      if (event.end && event.type == DataEventType::kDone) {
        terminal_done = true;
      }
    }
    sampler.SampleParent();
    if (child_pid.has_value()) {
      sampler.SampleChild(*child_pid);
    }
  }
  const auto session_end = Clock::now();

  const auto stopped = profile.stop();
  std::error_code ignored;
  std::filesystem::remove(endpoint_file, ignored);

  sample.session_latency_us = ElapsedUs(session_begin, session_end);
  sample.parent_rss_peak_kb = sampler.parent_peak_kb();
  sample.child_rss_peak_kb = sampler.child_peak_kb();
  sample.child_rss_measured = sampler.child_measured();
  const auto stats = profile.status().proxy;
  sample.input_queue_peak = stats.input_queue_peak;
  sample.output_queue_peak = stats.output_queue_peak;
  sample.pcm_frames = pcm_frames;
  sample.text = text;

  sample.success = stream_started && pushed && input_finished && terminal_done &&
                   stopped.ok() && !text.empty() && pcm_frames > 0;
  if (!sample.success) {
    if (!error.empty()) {
      sample.error = error;
    } else if (!stopped.ok()) {
      sample.error = stopped.error.message;
    } else {
      sample.error = "多进程轮次没有形成完整成功终态";
    }
  }
  return sample;
}

std::string Fnv1aHex(std::string_view input) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char byte : input) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  std::ostringstream stream;
  stream << std::hex << std::setw(16) << std::setfill('0') << hash;
  return stream.str();
}

std::string CanonicalConfig() {
  std::ostringstream stream;
  stream << "version=" << NEXWEAVE_VERSION_STRING_FROM_CMAKE << "\n";
  stream << "commit=" << NEXWEAVE_BUILD_GIT_COMMIT << "\n";
  stream << "input_frames=" << kInputValues.size() << "\n";
  stream << "input_samples=";
  for (const std::int16_t value : kInputValues) {
    stream << value << ",";
  }
  stream << "\nframe_samples=" << kFrameSamples << "\n";
  stream << "frame_bytes=" << kFrameBytes << "\n";
  stream << "backend=fake_asr:远端会话回答|fake_llm:远端,会话,回答|fake_tts|fake_rag:empty\n";
  stream << "source_queue=64\nplayback_queue=256\n";
  stream << "proxy_input_queue=64\nproxy_output_queue=256\n";
  stream << "segmentation=preroll0,min_speech1,min_silence2,max_speech1000\n";
  stream << "playback_clock=logical-ticking\n";
  return stream.str();
}

std::uint64_t Percentile(std::vector<std::uint64_t> values, double percentile) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  const double rank = std::ceil(percentile * static_cast<double>(values.size()));
  const std::size_t index = rank <= 1.0
                                ? 0
                                : static_cast<std::size_t>(rank) - 1;
  return values[std::min(index, values.size() - 1)];
}

Json Summarize(const std::vector<Sample>& samples, bool child_rss_expected) {
  Json summary;
  std::vector<std::uint64_t> latencies;
  std::vector<std::uint64_t> parent_rss;
  std::vector<std::uint64_t> child_rss;
  std::vector<std::uint64_t> node_start;
  std::size_t successes = 0;
  for (const Sample& sample : samples) {
    if (sample.success) {
      ++successes;
      latencies.push_back(sample.session_latency_us);
      parent_rss.push_back(sample.parent_rss_peak_kb);
      if (sample.child_rss_measured) {
        child_rss.push_back(sample.child_rss_peak_kb);
      }
      if (sample.node_start_latency_us > 0) {
        node_start.push_back(sample.node_start_latency_us);
      }
    }
  }
  summary["samples"] = samples.size();
  summary["success"] = successes;
  summary["success_rate"] =
      samples.empty() ? 0.0
                      : static_cast<double>(successes) / static_cast<double>(samples.size());
  summary["session_latency_us"] = {
      {"min", latencies.empty() ? 0 : *std::min_element(latencies.begin(), latencies.end())},
      {"p50", Percentile(latencies, 0.50)},
      {"p95", Percentile(latencies, 0.95)},
      {"max", latencies.empty() ? 0 : *std::max_element(latencies.begin(), latencies.end())}};
  summary["parent_rss_peak_kb"] = {
      {"p50", Percentile(parent_rss, 0.50)},
      {"p95", Percentile(parent_rss, 0.95)},
      {"max", parent_rss.empty() ? 0 : *std::max_element(parent_rss.begin(), parent_rss.end())}};
  if (!child_rss.empty()) {
    summary["child_rss_peak_kb"] = {
        {"p50", Percentile(child_rss, 0.50)},
        {"p95", Percentile(child_rss, 0.95)},
        {"max", *std::max_element(child_rss.begin(), child_rss.end())}};
  } else if (child_rss_expected) {
    summary["child_rss_peak_kb"] = nullptr;
  }
  summary["node_start_latency_us"] = {
      {"p50", Percentile(node_start, 0.50)},
      {"p95", Percentile(node_start, 0.95)},
      {"max", node_start.empty() ? 0 : *std::max_element(node_start.begin(), node_start.end())}};
  return summary;
}

Json SampleJson(const Sample& sample) {
  Json value;
  value["round"] = sample.round;
  value["success"] = sample.success;
  value["error"] = sample.error;
  value["session_latency_us"] = sample.session_latency_us;
  value["node_start_latency_us"] = sample.node_start_latency_us;
  value["parent_rss_peak_kb"] = sample.parent_rss_peak_kb;
  if (sample.child_rss_measured) {
    value["child_rss_peak_kb"] = sample.child_rss_peak_kb;
  } else {
    value["child_rss_peak_kb"] = nullptr;
  }
  value["input_queue_peak"] = sample.input_queue_peak;
  value["output_queue_peak"] = sample.output_queue_peak;
  value["pcm_frames"] = sample.pcm_frames;
  value["text"] = sample.text;
  return value;
}

struct Options {
  std::size_t warmup_rounds = kDefaultWarmupRounds;
  std::size_t measured_rounds = kDefaultMeasuredRounds;
  std::string summary_path;
};

bool ParseSize(const std::string& text, std::size_t& value) {
  try {
    const long long parsed = std::stoll(text);
    if (parsed < 0) {
      return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseOptions(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    if (flag == "--rounds") {
      if (index + 1 >= argc || !ParseSize(argv[index + 1], options.measured_rounds)) {
        std::cerr << "--rounds 需要非负整数" << std::endl;
        return false;
      }
      ++index;
    } else if (flag == "--warmup") {
      if (index + 1 >= argc || !ParseSize(argv[index + 1], options.warmup_rounds)) {
        std::cerr << "--warmup 需要非负整数" << std::endl;
        return false;
      }
      ++index;
    } else if (flag == "--summary") {
      if (index + 1 >= argc) {
        std::cerr << "--summary 需要路径" << std::endl;
        return false;
      }
      options.summary_path = argv[index + 1];
      ++index;
    } else if (flag == "--help") {
      std::cout << "用法: linux_profile_comparison_test [--warmup N] [--rounds N] "
                   "[--summary path]\n";
      return false;
    } else {
      std::cerr << "未知参数: " << flag << std::endl;
      return false;
    }
  }
  if (options.measured_rounds == 0 || options.measured_rounds > kMaxRounds ||
      options.warmup_rounds > kMaxRounds) {
    std::cerr << "轮次数必须在 1.." << kMaxRounds << " 之间" << std::endl;
    return false;
  }
  return true;
}

int Run(const Options& options) {
  Json root;
  root["test"] = "linux_profile_comparison";
  root["version"] = NEXWEAVE_VERSION_STRING_FROM_CMAKE;
  root["build_commit"] = NEXWEAVE_BUILD_GIT_COMMIT;
  root["build_cmake"] = NEXWEAVE_BUILD_CMAKE_VERSION;
  root["topology_variable"] =
      "same SessionApp fake assembly: in-process run vs LinuxProfile child process";
  root["timing_scope"] =
      "session_latency_us starts when the first input action is submitted and ends at the "
      "successful terminal; multi-process node_start_latency_us is recorded separately";
  root["percentile_method"] = "nearest-rank ceil(p*n)-1";
  root["warmup_rounds"] = options.warmup_rounds;
  root["measured_rounds"] = options.measured_rounds;
  root["input"] = {
      {"frame_samples", kFrameSamples},
      {"frame_bytes", kFrameBytes},
      {"frame_ms", nexweave::domain::kAudioFrameDurationMs},
      {"values", kInputValues}};
  root["queues"] = {
      {"source_capacity_frames", 64},
      {"playback_capacity_frames", 256},
      {"proxy_input_capacity_events", 64},
      {"proxy_output_capacity_events", 256}};
  root["config_hash"] = Fnv1aHex(CanonicalConfig());
  root["backend_group"] = "deterministic_fake";
  root["known_limits"] = Json::array({
      "Fake 调度证据，不代表模型、NPU、ALSA 或板端性能",
      "RSS 是 /proc/<pid>/status 的峰值采样，不区分共享页；父子进程 RSS 不可简单相加",
      "session_latency 依赖主机调度、编译器和当前负载，不是设备端到端延迟",
      "单进程路径没有独立节点启动阶段，因此 node_start_latency_us 固定为 0，不能与多进程启动耗时直接相减"});
  root["unmeasured"] = Json::array({
      "真实模型首 token、首 PCM、RTF 与 NPU 占用",
      "真实网络断连、重连和半开连接下的 profile 延迟",
      "目标板 P50/P95/P99、CPU/RSS 长稳趋势与实际静音时间"});

  for (std::size_t index = 0; index < options.warmup_rounds; ++index) {
    (void)RunSingleProcess(index);
    (void)RunMultiProcess(index);
  }

  std::vector<Sample> single_samples;
  std::vector<Sample> multi_samples;
  Json samples = Json::array();
  std::size_t output_matches = 0;
  for (std::size_t index = 0; index < options.measured_rounds; ++index) {
    Sample single;
    Sample multi;
    if (index % 2 == 0) {
      single = RunSingleProcess(index);
      multi = RunMultiProcess(index);
    } else {
      multi = RunMultiProcess(index);
      single = RunSingleProcess(index);
    }
    const bool output_match = single.success && multi.success &&
                              single.text == multi.text &&
                              single.pcm_frames == multi.pcm_frames;
    if (output_match) {
      ++output_matches;
    }
    single_samples.push_back(single);
    multi_samples.push_back(multi);
    samples.push_back({
        {"round", index + 1},
        {"single", SampleJson(single)},
        {"multi", SampleJson(multi)},
        {"output_match", output_match}});
  }

  root["samples"] = std::move(samples);
  root["summary"]["single"] = Summarize(single_samples, false);
  root["summary"]["multi"] = Summarize(multi_samples, true);
  root["output_matches"] = output_matches;

  const std::size_t single_success =
      static_cast<std::size_t>(std::count_if(single_samples.begin(), single_samples.end(),
                                             [](const Sample& sample) { return sample.success; }));
  const std::size_t multi_success =
      static_cast<std::size_t>(std::count_if(multi_samples.begin(), multi_samples.end(),
                                             [](const Sample& sample) { return sample.success; }));
  const bool passed = output_matches == options.measured_rounds &&
                      single_success == options.measured_rounds &&
                      multi_success == options.measured_rounds;
  root["passed"] = passed;

  const std::string encoded = root.dump(2);
  if (!options.summary_path.empty()) {
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(options.summary_path).parent_path(), error);
    std::ofstream file(options.summary_path);
    if (!file.is_open()) {
      std::cerr << "无法写入汇总文件: " << options.summary_path << std::endl;
      return 1;
    }
    file << encoded << '\n';
  }
  std::cout << encoded << std::endl;
  return passed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseOptions(argc, argv, options)) {
    return 1;
  }
  return Run(options);
}

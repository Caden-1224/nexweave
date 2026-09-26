// RK3576 板端部署门禁：用受控子进程夹具重复验证启动、就绪、停止和失败回滚机制。
//
// 该可执行文件不加载真实模型、不打开 ALSA；它只验证 45 要求的进程拓扑、音频唯一拥有者、
// 启动顺序、回滚和资源释放。任务 48/49 汇合真实构件后必须在同一入口上再次运行，不能把本
// 文件的 Fake 通过写成完整离线链路的结论。
#include "rk3576_profile.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef NEXWEAVE_DEPLOYMENT_NODE_FIXTURE
#error "硬件部署门禁需要 NEXWEAVE_DEPLOYMENT_NODE_FIXTURE 指向部署节点夹具"
#endif

namespace {

using namespace std::chrono_literals;
using nexweave::app::Rk3576Profile;
using nexweave::app::Rk3576ProfileConfig;
using nexweave::app::Rk3576ProfileState;
using nexweave::app::Rk3576ProcessSpec;
using nexweave::runtime::ChildProcess;
using nexweave::runtime::ChildProcessExit;
using nexweave::runtime::ChildProcessIdentity;
using nexweave::runtime::ChildProcessSpec;

std::string MakeTempDirectory() {
  char path[] = "/tmp/nexweave-rk3576-hardware-XXXXXX";
  char* directory = ::mkdtemp(path);
  if (directory == nullptr) {
    throw std::runtime_error("无法创建板端部署测试临时目录");
  }
  return directory;
}

class TempDirectory {
 public:
  TempDirectory() : path_(MakeTempDirectory()) {}
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::string& path() const {
    return path_;
  }

 private:
  std::string path_;
};

struct Options {
  std::string mode = "success";
  std::string work_directory;
  std::string fixture = NEXWEAVE_DEPLOYMENT_NODE_FIXTURE;
  int repeat = 1;
};

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--fixture" && index + 1 < argc) {
      options.fixture = argv[++index];
    } else if (argument == "--work-dir" && index + 1 < argc) {
      options.work_directory = argv[++index];
    } else if (argument == "--mode" && index + 1 < argc) {
      options.mode = argv[++index];
    } else if (argument == "--repeat" && index + 1 < argc) {
      options.repeat = std::atoi(argv[++index]);
    } else {
      throw std::runtime_error("未知参数: " + argument);
    }
  }
  if (options.repeat <= 0) {
    throw std::runtime_error("--repeat 必须为正整数");
  }
  if (options.mode != "success" && options.mode != "partial-failure" &&
      options.mode != "contention" && options.mode != "kill-escalation") {
    throw std::runtime_error("未知模式: " + options.mode);
  }
  return options;
}

std::string AdapterPath(const Options& options) {
  if (!options.work_directory.empty()) {
    return options.work_directory + "/audio-frontend";
  }
  return "/tmp/nexweave-rk3576-hardware-" + std::to_string(::getpid()) + "/audio-frontend";
}

Rk3576ProcessSpec MakeNode(const Options& options,
                           const std::string& name,
                           const std::string& adapter_path,
                           const std::string& mode,
                           bool owns_audio,
                           bool uses_audio_adapter,
                           std::chrono::milliseconds start_budget = 1000ms) {
  Rk3576ProcessSpec spec;
  spec.name = name;
  spec.process.executable = options.fixture;
  spec.process.arguments = {"--name", name, "--adapter", adapter_path, "--mode", mode};
  if (owns_audio) {
    spec.process.arguments.push_back("--owns-audio");
  }
  if (uses_audio_adapter) {
    spec.process.arguments.push_back("--uses-audio-adapter");
  }
  spec.process.expect_ready_signal = true;
  spec.lifecycle.start_wait_budget = start_budget;
  spec.lifecycle.stop_wait_budget = 200ms;
  spec.lifecycle.kill_wait_budget = 500ms;
  spec.lifecycle.poll_interval = 2ms;
  spec.owns_audio_device = owns_audio;
  spec.uses_audio_adapter = uses_audio_adapter;
  return spec;
}

Rk3576ProfileConfig MakeSuccessConfig(const Options& options,
                                      const std::string& adapter_path) {
  Rk3576ProfileConfig config;
  Rk3576ProcessSpec owner =
      MakeNode(options, "audio-frontend", adapter_path, "hold", true, false);
  Rk3576ProcessSpec asr = MakeNode(options, "asr", adapter_path, "hold", false, true);
  asr.start_after = {"audio-frontend"};
  Rk3576ProcessSpec llm = MakeNode(options, "llm", adapter_path, "hold", false, true);
  llm.start_after = {"audio-frontend"};
  Rk3576ProcessSpec session =
      MakeNode(options, "session", adapter_path, "hold", false, true);
  session.start_after = {"asr", "llm"};
  config.processes = {std::move(owner), std::move(asr), std::move(llm),
                      std::move(session)};
  return config;
}

bool ResourcesReleased(const std::string& adapter_path) {
  return !std::filesystem::exists(adapter_path + ".lock") &&
         !std::filesystem::exists(adapter_path + ".sock");
}

void RequireReleased(const std::string& adapter_path) {
  if (!ResourcesReleased(adapter_path)) {
    throw std::runtime_error("部署停止后仍有音频所有权或适配器 socket 残留");
  }
}

int RunSuccess(const Options& options, const std::string& adapter_path) {
  Rk3576Profile profile(MakeSuccessConfig(options, adapter_path));
  for (int round = 0; round < options.repeat; ++round) {
    const auto started = round == 0 ? profile.start() : profile.restart();
    if (!started.ok()) {
      std::cerr << "第 " << round + 1 << " 轮启动或重启失败: "
                << started.error.message << "\n";
      return 1;
    }
    const auto status = profile.status();
    if (status.state != Rk3576ProfileState::kReady || !status.audio_owner_ready ||
        !status.audio_adapter_ready) {
      std::cerr << "第 " << round + 1 << " 轮未达到就绪顺序要求\n";
      return 1;
    }
    if (!std::filesystem::exists(adapter_path + ".lock") ||
        !std::filesystem::exists(adapter_path + ".sock")) {
      std::cerr << "第 " << round + 1 << " 轮音频拥有者未建立独占资源\n";
      return 1;
    }
    const auto stopped = profile.stop();
    if (!stopped.ok()) {
      std::cerr << "第 " << round + 1 << " 轮停止失败: " << stopped.error.message << "\n";
      return 1;
    }
    RequireReleased(adapter_path);
  }
  return 0;
}

int RunPartialFailure(const Options& options, const std::string& adapter_path) {
  Rk3576ProfileConfig config;
  Rk3576ProcessSpec owner =
      MakeNode(options, "audio-frontend", adapter_path, "hold", true, false);
  Rk3576ProcessSpec failing =
      MakeNode(options, "asr", adapter_path, "exit-before-ready", false, true);
  failing.start_after = {"audio-frontend"};
  config.processes = {std::move(owner), std::move(failing)};
  Rk3576Profile profile(std::move(config));
  const auto started = profile.start();
  if (started.ok()) {
    std::cerr << "部分启动失败用例被错误判定为成功\n";
    return 1;
  }
  if (profile.status().state != Rk3576ProfileState::kIdle) {
    std::cerr << "部分启动失败后回滚未回到空闲状态\n";
    return 1;
  }
  RequireReleased(adapter_path);
  if (!profile.stop().ok()) {
    std::cerr << "部分启动失败后的清理重试失败\n";
    return 1;
  }
  return 0;
}

int RunContention(const Options& options, const std::string& adapter_path) {
  ChildProcess external;
  ChildProcessSpec external_spec;
  external_spec.executable = options.fixture;
  external_spec.arguments = {"--name", "external-owner", "--adapter", adapter_path,
                             "--mode", "hold", "--owns-audio"};
  external_spec.inherit_environment = true;
  external_spec.expect_ready_signal = true;
  const auto external_started = external.start(external_spec);
  if (!external_started.ok()) {
    std::cerr << "无法启动外部音频拥有者: " << external_started.error.message << "\n";
    return 1;
  }

  Rk3576ProfileConfig config;
  Rk3576ProcessSpec owner =
      MakeNode(options, "audio-frontend", adapter_path, "hold", true, false);
  Rk3576ProcessSpec session =
      MakeNode(options, "session", adapter_path, "hold", false, true);
  session.start_after = {"audio-frontend"};
  config.processes = {std::move(owner), std::move(session)};
  Rk3576Profile profile(std::move(config));
  if (!std::filesystem::exists(adapter_path + ".lock") ||
      !std::filesystem::exists(adapter_path + ".sock")) {
    std::cerr << "外部音频拥有者没有建立独占资源\n";
    return 1;
  }

  const auto started = profile.start();
  if (started.ok()) {
    std::cerr << "第二个音频拥有者错误地取得设备所有权\n";
    return 1;
  }

  const auto external_stopped = external.stop(*external_started.value);
  if (!external_stopped.ok()) {
    std::cerr << "外部音频拥有者清理失败\n";
    return 1;
  }
  RequireReleased(adapter_path);
  return 0;
}

int RunKillEscalation(const Options& options, const std::string& adapter_path) {
  Rk3576ProfileConfig config;
  Rk3576ProcessSpec owner =
      MakeNode(options, "audio-frontend", adapter_path, "hold", true, false);
  Rk3576ProcessSpec stubborn =
      MakeNode(options, "session", adapter_path, "ignore-term", false, true);
  stubborn.start_after = {"audio-frontend"};
  stubborn.lifecycle.stop_wait_budget = 50ms;
  stubborn.lifecycle.kill_wait_budget = 500ms;
  config.processes = {std::move(owner), std::move(stubborn)};
  Rk3576Profile profile(std::move(config));
  if (!profile.start().ok()) {
    std::cerr << "SIGKILL 升级用例启动失败\n";
    return 1;
  }
  if (!profile.stop().ok()) {
    std::cerr << "SIGKILL 升级用例停止失败\n";
    return 1;
  }
  RequireReleased(adapter_path);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = ParseOptions(argc, argv);
    TempDirectory temp;
    const std::string adapter_path =
        options.work_directory.empty() ? temp.path() + "/audio-frontend" : AdapterPath(options);

    int result = 1;
    if (options.mode == "success") {
      result = RunSuccess(options, adapter_path);
    } else if (options.mode == "partial-failure") {
      result = RunPartialFailure(options, adapter_path);
    } else if (options.mode == "contention") {
      result = RunContention(options, adapter_path);
    } else if (options.mode == "kill-escalation") {
      result = RunKillEscalation(options, adapter_path);
    }

    if (result == 0) {
      std::cout << "rk3576_deployment_hardware_test"
                << " mode=" << options.mode
                << " repeat=" << options.repeat
                << " result=pass"
                << " start_wait_ms=1000"
                << " stop_wait_ms=200"
                << " kill_wait_ms=500"
                << "\n";
    }
    return result;
  } catch (const std::exception& error) {
    std::cerr << "rk3576_deployment_hardware_test failed: " << error.what() << "\n";
    return 1;
  }
}

// 子进程生命周期测试夹具。它只实现几种可由参数选择的确定性行为：显式就绪、延迟到触发
// 文件出现后就绪、忽略 SIGTERM、正常退出、崩溃退出。这样父进程测试可以观察启动就绪、
// 停止升级和退出归档，而不依赖真实模型或设备。
#include "child_process.hpp"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

void IgnoreSigterm(int) {
  // 夹具故意不退出，以便测试 SIGTERM 超时后的 SIGKILL 升级路径。
}

bool FileExists(const std::string& path) {
  std::error_code error;
  return std::filesystem::exists(path, error);
}

void WaitForever() {
  while (true) {
    ::pause();
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "ready";

  if (mode == "trigger-ready") {
    if (argc < 3) {
      std::cerr << "缺少触发文件路径" << std::endl;
      return 2;
    }
    const std::string trigger = argv[2];
    while (!FileExists(trigger)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  } else if (mode == "never-ready") {
    WaitForever();
    return 0;
  } else if (mode == "never-ready-ignore") {
    std::signal(SIGTERM, IgnoreSigterm);
    WaitForever();
    return 0;
  } else if (mode == "exit-before-ready") {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    return 3;
  } else if (mode == "ready-spawn-grandchild") {
    if (argc < 3) {
      std::cerr << "缺少孙进程 PID 文件路径" << std::endl;
      return 2;
    }
    const pid_t grandchild = ::fork();
    if (grandchild < 0) {
      return 5;
    }
    if (grandchild == 0) {
      std::ofstream pid_file(argv[2]);
      pid_file << ::getpid() << std::endl;
      pid_file.close();
      WaitForever();
    }
  }

  if (mode == "ready-ignore-sigterm") {
    std::signal(SIGTERM, IgnoreSigterm);
  }

  const auto notified = nexweave::runtime::notify_parent_ready();
  if (!notified.ok()) {
    std::cerr << "就绪通知失败: " << notified.error.message << std::endl;
    return 4;
  }

  if (mode == "ready-exit") {
    const int code = argc > 2 ? std::atoi(argv[2]) : 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return code;
  }
  if (mode == "ready-crash") {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::abort();
  }

  WaitForever();
  return 0;
}

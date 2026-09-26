// 部署 profile 的受控子进程夹具：只模拟“节点就绪、音频设备唯一所有权和适配器连接”。
//
// 它不运行真实模型或 ALSA，只用于验证部署层的外部行为：
//   - 音频拥有者在 adapter.lock 上取得 O_EXCL 独占所有权，并 bind 一个 Unix socket；
//   - 适配器消费者必须连接到该 socket 才报告就绪，不能直接接触 lock；
//   - hold 模式等待 SIGTERM，并在退出时释放 socket 与 lock；
//   - no-ready/exit-before-ready 用于验证启动超时和部分启动失败回滚；
//   - crash-after-ready 用于验证就绪后异常退出的清理路径；
//   - ignore-term 用于验证停止预算耗尽后的 SIGKILL 升级。
//
// 该夹具只进入测试目标，不安装、不进入生产部署包。
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "child_process.hpp"

namespace {

volatile sig_atomic_t g_stop_requested = 0;

void HandleStopSignal(int) {
  g_stop_requested = 1;
}

std::string SocketPath(const std::string& adapter_path) {
  return adapter_path + ".sock";
}

std::string LockPath(const std::string& adapter_path) {
  return adapter_path + ".lock";
}

bool MakeParentDirectory(const std::string& path) {
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) {
    return true;
  }
  std::error_code error;
  std::filesystem::create_directories(parent, error);
  return !error;
}

int WriteProcessIdToLock(int lock_fd) {
  const std::string pid_text = std::to_string(static_cast<long long>(::getpid()));
  const ssize_t written = ::write(lock_fd, pid_text.data(), pid_text.size());
  return written == static_cast<ssize_t>(pid_text.size()) ? 0 : -1;
}

int OpenExclusiveLock(const std::string& adapter_path, int* lock_fd) {
  if (!MakeParentDirectory(adapter_path)) {
    std::cerr << "无法创建适配器目录: " << adapter_path << "\n";
    return -1;
  }
  *lock_fd = ::open(LockPath(adapter_path).c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (*lock_fd < 0) {
    std::cerr << "音频设备所有权已被占用: " << std::strerror(errno) << "\n";
    return -1;
  }
  if (WriteProcessIdToLock(*lock_fd) != 0) {
    ::close(*lock_fd);
    *lock_fd = -1;
    ::unlink(LockPath(adapter_path).c_str());
    std::cerr << "写入音频设备所有权文件失败\n";
    return -1;
  }
  return 0;
}

int ListenOnAdapterSocket(const std::string& adapter_path, int* socket_fd) {
  *socket_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (*socket_fd < 0) {
    std::cerr << "创建音频适配器 socket 失败: " << std::strerror(errno) << "\n";
    return -1;
  }

  const std::string socket_path = SocketPath(adapter_path);
  ::unlink(socket_path.c_str());
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(address.sun_path)) {
    std::cerr << "音频适配器 socket 路径过长\n";
    ::close(*socket_fd);
    *socket_fd = -1;
    return -1;
  }
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  if (::bind(*socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    std::cerr << "绑定音频适配器 socket 失败: " << std::strerror(errno) << "\n";
    ::close(*socket_fd);
    *socket_fd = -1;
    return -1;
  }
  if (::listen(*socket_fd, 8) != 0) {
    std::cerr << "监听音频适配器 socket 失败: " << std::strerror(errno) << "\n";
    ::close(*socket_fd);
    *socket_fd = -1;
    ::unlink(socket_path.c_str());
    return -1;
  }
  return 0;
}

// 在有限预算内连接到音频拥有者 socket。每次失败后短暂等待，不把固定 sleep 当成就绪条件；
// 调用方已经把拥有者启动并等待就绪，因此这里的预算只处理本地调度延迟。
bool ConnectToAdapter(const std::string& adapter_path, std::chrono::milliseconds budget) {
  const std::string socket_path = SocketPath(adapter_path);
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
      ::close(fd);
      return false;
    }
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      ::close(fd);
      return false;
    }

    const int connected =
        ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (connected == 0) {
      ::close(fd);
      return true;
    }
    if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN) {
      ::close(fd);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    pollfd poll_fd{};
    poll_fd.fd = fd;
    poll_fd.events = POLLOUT;
    const int poll_result = ::poll(&poll_fd, 1, 20);
    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);
    const bool writable = poll_result > 0 && (poll_fd.revents & POLLOUT) != 0;
    const bool got_error =
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) == 0 &&
        socket_error != 0;
    ::close(fd);
    if (writable && !got_error) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

void ReleaseAdapterResources(const std::string& adapter_path,
                             bool owns_audio,
                             int lock_fd,
                             int socket_fd) {
  if (socket_fd >= 0) {
    ::close(socket_fd);
  }
  if (lock_fd >= 0) {
    ::close(lock_fd);
  }
  if (owns_audio && !adapter_path.empty()) {
    ::unlink(SocketPath(adapter_path).c_str());
    ::unlink(LockPath(adapter_path).c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string name = "deployment-node";
  std::string adapter_path;
  std::string mode = "hold";
  bool owns_audio = false;
  bool uses_audio_adapter = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--name" && index + 1 < argc) {
      name = argv[++index];
    } else if (argument == "--adapter" && index + 1 < argc) {
      adapter_path = argv[++index];
    } else if (argument == "--mode" && index + 1 < argc) {
      mode = argv[++index];
    } else if (argument == "--owns-audio") {
      owns_audio = true;
    } else if (argument == "--uses-audio-adapter") {
      uses_audio_adapter = true;
    } else {
      std::cerr << "未知参数: " << argument << "\n";
      return 2;
    }
  }

  if (name.empty()) {
    std::cerr << "缺少节点角色名\n";
    return 2;
  }
  if ((owns_audio || uses_audio_adapter) && adapter_path.empty()) {
    std::cerr << "音频节点必须提供 --adapter\n";
    return 2;
  }

  struct sigaction action {};
  action.sa_handler = HandleStopSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  ::sigaction(SIGTERM, &action, nullptr);
  ::sigaction(SIGINT, &action, nullptr);
  if (mode == "ignore-term") {
    ::signal(SIGTERM, SIG_IGN);
  }

  int lock_fd = -1;
  int socket_fd = -1;
  if (owns_audio) {
    if (OpenExclusiveLock(adapter_path, &lock_fd) != 0) {
      return 3;
    }
    if (ListenOnAdapterSocket(adapter_path, &socket_fd) != 0) {
      ReleaseAdapterResources(adapter_path, owns_audio, lock_fd, socket_fd);
      return 4;
    }
  } else if (uses_audio_adapter) {
    if (!ConnectToAdapter(adapter_path, std::chrono::milliseconds(2000))) {
      std::cerr << "无法连接音频适配器: " << adapter_path << "\n";
      return 5;
    }
  }

  if (mode == "exit-before-ready") {
    ReleaseAdapterResources(adapter_path, owns_audio, lock_fd, socket_fd);
    return 6;
  }

  if (mode != "no-ready") {
    const auto ready = nexweave::runtime::notify_parent_ready();
    if (!ready.ok()) {
      ReleaseAdapterResources(adapter_path, owns_audio, lock_fd, socket_fd);
      std::cerr << "通知父进程就绪失败: " << ready.error.message << "\n";
      return 7;
    }
  }

  if (mode == "crash-after-ready") {
    ReleaseAdapterResources(adapter_path, owns_audio, lock_fd, socket_fd);
    ::raise(SIGABRT);
    return 8;
  }

  while (!g_stop_requested) {
    ::pause();
  }

  ReleaseAdapterResources(adapter_path, owns_audio, lock_fd, socket_fd);
  return 0;
}

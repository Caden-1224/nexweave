// 多进程 Gateway 路由测试夹具：在独立进程中运行真实的 Gateway + Supervisor。
//
// 它只负责启动一个可控的远端控制服务：Supervisor 的拥有者在 run() 上停住，直到收到停止
// 请求才返回。这样父进程可以通过 ZeroMQ 控制路由观察“推理阻塞时查询/取消仍可达”。夹具
// 在控制端点绑定完成后调用 notify_parent_ready()，父进程因此不会把“进程已启动”误当
// “业务已就绪”。
#include "child_process.hpp"
#include "gateway.hpp"
#include "supervisor.hpp"
#include "zmq_control.hpp"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <fstream>

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void HandleSignal(int) {
  g_stop_requested = 1;
}

class BlockingOwner final : public nexweave::runtime::ISessionOwner {
 public:
  nexweave::domain::OperationResult start() override {
    return nexweave::domain::OperationResult::success();
  }

  nexweave::domain::OperationResult run() override {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return stop_requested_; });
    return nexweave::domain::OperationResult::failure(
        nexweave::domain::ErrorCode::kCancelled, "远端会话被停止");
  }

  nexweave::domain::OperationResult cleanup() noexcept override {
    return nexweave::domain::OperationResult::success();
  }

  void request_stop() noexcept override {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      stop_requested_ = true;
    }
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stop_requested_ = false;
};

class BlockingOwnerFactory final : public nexweave::runtime::ISessionOwnerFactory {
 public:
  std::shared_ptr<nexweave::runtime::ISessionOwner> create(
      const nexweave::runtime::SupervisorSessionSpec& spec,
      nexweave::domain::Error& error) override {
    (void)spec;
    error = nexweave::domain::Error{};
    return std::make_shared<BlockingOwner>();
  }
};

class EmptyRunSource final : public nexweave::runtime::ISessionRunSource {
 public:
  std::shared_ptr<const nexweave::runtime::SessionAppRunRecord> last_run() const override {
    return nullptr;
  }
};

bool WriteEndpointFile(const std::string& path, const std::string& endpoint) {
  std::ofstream file(path);
  if (!file.is_open()) {
    return false;
  }
  file << endpoint << std::endl;
  return file.good();
}

std::string ParseEndpointFile(int argc, char** argv) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::string(argv[index]) == "--endpoint-file") {
      return argv[index + 1];
    }
  }
  return std::string();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string endpoint_file = ParseEndpointFile(argc, argv);
  if (endpoint_file.empty()) {
    std::cerr << "缺少 --endpoint-file" << std::endl;
    return 2;
  }

  std::signal(SIGTERM, HandleSignal);
  std::signal(SIGINT, HandleSignal);

  BlockingOwnerFactory factory;
  nexweave::runtime::Supervisor supervisor(factory);
  EmptyRunSource run_source;
  nexweave::gateway::Gateway gateway(supervisor, run_source);

  nexweave::transport::ZmqControlServerConfig server_config;
  server_config.endpoint = "tcp://127.0.0.1:*";
  nexweave::transport::ZmqControlServer server(gateway, server_config);
  const nexweave::domain::OperationResult started = server.start();
  if (!started.ok()) {
    std::cerr << "控制服务启动失败: " << started.error.message << std::endl;
    return 3;
  }

  if (!WriteEndpointFile(endpoint_file, server.bound_endpoint())) {
    std::cerr << "写入端点文件失败" << std::endl;
    server.stop();
    return 4;
  }

  const nexweave::domain::OperationResult notified =
      nexweave::runtime::notify_parent_ready();
  if (!notified.ok()) {
    std::cerr << "就绪通知失败: " << notified.error.message << std::endl;
    server.stop();
    return 5;
  }

  while (g_stop_requested == 0) {
    ::pause();
  }

  server.stop();
  return 0;
}

#include "../test_support.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "session_app_owner.hpp"
#include "zmq_control.hpp"

using namespace nexweave;
using namespace std::chrono_literals;

namespace {

using Milliseconds = std::chrono::milliseconds;

constexpr Milliseconds kControlDeadline{2000};
constexpr Milliseconds kWaitBound{4000};

// 闸门把“会话正在执行”从瞬时状态变成可把握的窗口；它只做同步，不拥有资源。
class Gate {
 public:
  void hold() {
    const std::lock_guard<std::mutex> guard(mutex_);
    held_ = true;
  }

  void release() {
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      held_ = false;
    }
    condition_.notify_all();
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return !held_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool held_ = false;
};

// 阻塞拥有者：start 立即成功，run 停在闸门上直到测试放行。它模拟“推理仍在执行”，
// 不访问设备、文件或模型，因此测试可以在毫秒级内稳定复现控制并发窗口。
class BlockingOwner final : public runtime::ISessionOwner {
 public:
  class Factory;

  BlockingOwner(Factory& factory, const runtime::SupervisorSessionSpec& spec)
      : factory_(factory), spec_(spec) {}

  domain::OperationResult start() override { return domain::OperationResult::success(); }
  domain::OperationResult run() override;
  domain::OperationResult cleanup() noexcept override {
    return domain::OperationResult::success();
  }
  void request_stop() noexcept override;

 private:
  Factory& factory_;
  runtime::SupervisorSessionSpec spec_;
};

class BlockingOwner::Factory final : public runtime::ISessionOwnerFactory,
                                     public runtime::ISessionRunSource {
 public:
  explicit Factory(Gate& run_gate) : run_gate_(run_gate) {}

  void hold_run() { run_gate_.hold(); }
  void release_run() { run_gate_.release(); }
  void wait_run() { run_gate_.wait(); }

  std::shared_ptr<runtime::ISessionOwner> create(
      const runtime::SupervisorSessionSpec& spec, domain::Error& error) override {
    error = domain::Error{};
    return std::make_shared<BlockingOwner>(*this, spec);
  }

  std::shared_ptr<const runtime::SessionAppRunRecord> last_run() const override {
    return nullptr;
  }

  std::atomic<int> stop_requests{0};

 private:
  Gate& run_gate_;
};

domain::OperationResult BlockingOwner::run() {
  factory_.wait_run();
  return domain::OperationResult::success();
}

void BlockingOwner::request_stop() noexcept {
  factory_.stop_requests.fetch_add(1);
}

protocol::ControlRequest Request(const std::string& request_id,
                                 const std::string& operation,
                                 Milliseconds deadline = kControlDeadline) {
  protocol::ControlRequest request;
  request.request_id = request_id;
  request.operation = operation;
  request.deadline = deadline;
  return request;
}

// 夹具按“工厂 → 监督器 → 请求入口 → 传输服务端 → 客户端”的借用顺序构造，析构时先放行
// 会话、再停止传输、最后让监督器收敛。闸门放行放在析构最前面，保证断言失败抛出异常时
// 不会把拥有者线程永远卡在条件变量上。
struct Fixture {
  Gate run_gate;
  BlockingOwner::Factory factory;
  runtime::Supervisor supervisor;
  gateway::Gateway gateway;
  std::unique_ptr<transport::ZmqControlServer> server;
  std::unique_ptr<transport::ZmqControlClient> client;

  Fixture()
      : factory(run_gate),
        supervisor(factory, runtime::SupervisorConfig{Milliseconds{2000}, Milliseconds{2000}}),
        gateway(supervisor, factory, MakeGatewayConfig()) {}

  ~Fixture() {
    run_gate.release();
    if (client) {
      client->close();
    }
    if (server) {
      server->stop();
    }
    supervisor.shutdown(Milliseconds{2000});
  }

  static gateway::GatewayConfig MakeGatewayConfig() {
    gateway::GatewayConfig config;
    // 取消只验证“受理”，不把清理是否完成混进本轮控制可达性；出口预算也给一个小上界。
    config.cancel_wait_budget = Milliseconds{0};
    config.exit_wait_budget = Milliseconds{100};
    return config;
  }

  domain::OperationResult StartServerAndClient() {
    transport::ZmqControlServerConfig server_config;
    server_config.poll_interval = Milliseconds{5};
    server_config.send_timeout = Milliseconds{200};
    server_config.bind_wait_budget = Milliseconds{1000};
    server.reset(new transport::ZmqControlServer(gateway, server_config));
    const auto started = server->start();
    if (!started.ok()) {
      return started;
    }
    const std::string endpoint = server->bound_endpoint();
    if (endpoint.empty()) {
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                              "服务端没有返回绑定端点");
    }
    transport::ZmqControlClientConfig client_config;
    client_config.send_timeout = Milliseconds{200};
    client.reset(new transport::ZmqControlClient(endpoint, client_config));
    return client->connect();
  }

  bool WaitForIdle() {
    const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
    while (supervisor.status().state != runtime::SupervisorState::kIdle) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(Milliseconds{1});
    }
    return true;
  }
};

std::string Fact(const std::string& message, const std::string& key) {
  std::size_t position = 0;
  while (position < message.size()) {
    const std::size_t space = message.find(' ', position);
    const std::string token = message.substr(
        position, space == std::string::npos ? std::string::npos : space - position);
    const std::size_t equals = token.find('=');
    if (equals != std::string::npos && token.substr(0, equals) == key) {
      return token.substr(equals + 1);
    }
    if (space == std::string::npos) {
      break;
    }
    position = space + 1;
  }
  return {};
}

// 保护不变量：会话已经进入 run 并阻塞时，传输线程仍能接收并完成 query/cancel；控制
// 请求不会等到推理结束才被处理。start 的受理与 cancel 的受理必须分开观察。
void TestControlRemainsReachableWhileSessionRuns() {
  Fixture fixture;
  fixture.factory.hold_run();
  CHECK(fixture.StartServerAndClient().ok());

  const auto started = fixture.client->call(Request("start-running", "start"));
  CHECK(started.ok());
  CHECK(started.value->result.ok());

  const auto queried = fixture.client->call(Request("query-running", "query"));
  CHECK(queried.ok());
  CHECK(queried.value->result.ok());
  CHECK(queried.value->result.error.message.find("state=active") != std::string::npos);

  const auto cancelled = fixture.client->call(Request("cancel-running", "cancel"));
  CHECK(cancelled.ok());
  CHECK(Fact(cancelled.value->result.error.message, "accepted") == "1");

  fixture.factory.release_run();
  CHECK(fixture.WaitForIdle());
}

// 保护不变量：同一个 request_id 在会话执行中重试不会启动第二会话；服务端返回已受理，
// 但不把“重复请求”伪装成成功的新执行。
void TestDuplicateRequestDoesNotStartASecondSession() {
  Fixture fixture;
  fixture.factory.hold_run();
  CHECK(fixture.StartServerAndClient().ok());

  const auto first = fixture.client->call(Request("start-duplicate", "start"));
  CHECK(first.ok());
  CHECK(first.value->result.ok());

  const auto second = fixture.client->call(Request("start-duplicate", "start"));
  CHECK(second.ok());
  CHECK(second.value->result.error.code == domain::ErrorCode::kAlreadyCompleted);
  CHECK(fixture.supervisor.status().sessions_started == 1);

  fixture.factory.release_run();
  CHECK(fixture.WaitForIdle());
}

// 保护不变量：服务端可以重复启动/停止；每次 stop 后线程与 socket 都已回收，下一次
// start 绑定新的临时端点而不是复用已关闭的 socket。回环临时端口让测试不争抢固定端口。
void TestRepeatedStartStopIsClean() {
  Fixture fixture;
  for (int attempt = 0; attempt < 3; ++attempt) {
    transport::ZmqControlServerConfig config;
    config.poll_interval = Milliseconds{5};
    config.send_timeout = Milliseconds{200};
    config.bind_wait_budget = Milliseconds{1000};
    fixture.server.reset(new transport::ZmqControlServer(fixture.gateway, config));
    CHECK(fixture.server->start().ok());
    CHECK(fixture.server->running());
    CHECK(!fixture.server->bound_endpoint().empty());
    CHECK(fixture.server->stop().ok());
    CHECK(!fixture.server->running());
  }
}

// 保护不变量：没有服务端时客户端在请求预算内明确返回超时，不伪造成功，也不把一次
// 连接失败变成重复执行。端口 1 只用于制造“对端不存在”，不参与任何确定性数据路径。
void TestClientTimeoutIsStructured() {
  transport::ZmqControlClientConfig config;
  config.send_timeout = Milliseconds{50};
  transport::ZmqControlClient client("tcp://127.0.0.1:1", config);
  const auto response =
      client.call(Request("timeout-case", "query", Milliseconds{100}));
  CHECK(!response.ok());
  CHECK(response.error.code == domain::ErrorCode::kTimeout);
}

// 保护不变量：stop 由非 I/O 线程调用时能收敛；若服务端正持有在途会话，关闭 Gateway
// 虚拟连接会走“对端断开”策略受理取消，而不是静默丢弃会话或留下线程。
void TestStopWhileSessionIsRunningReleasesThread() {
  Fixture fixture;
  fixture.factory.hold_run();
  CHECK(fixture.StartServerAndClient().ok());
  const auto started = fixture.client->call(Request("start-stop", "start"));
  CHECK(started.ok());
  CHECK(started.value->result.ok());

  CHECK(fixture.server->stop().ok());
  CHECK(!fixture.server->running());
  CHECK(fixture.factory.stop_requests.load() == 1);

  fixture.factory.release_run();
  CHECK(fixture.WaitForIdle());
}

// 保护不变量：服务端停掉后客户端的请求在预算内明确超时；服务端以同一端点和同一
// Gateway 重新启动后，客户端重连并用同一 request_id 重试，Gateway 重放已完成响应而
// 不重新执行查询。这样“超时后重建”与“重复请求不重复执行”在同一条路径上被覆盖。
void TestClientReconnectsAfterServerRestartAndReplaysRequest() {
  Fixture fixture;
  CHECK(fixture.StartServerAndClient().ok());
  const std::string endpoint = fixture.server->bound_endpoint();

  // 重试必须保持同一逻辑请求的全部字段；deadline 也是幂等比较的一部分，因此三次调用
  // 复用同一个 request 对象，而不是只复用 request_id。
  const auto original = Request("restart-query", "query", Milliseconds{500});
  const auto first = fixture.client->call(original);
  CHECK(first.ok());
  CHECK(first.value->result.ok());
  CHECK(!first.value->replayed);
  CHECK(fixture.server->stop().ok());

  const auto timed_out = fixture.client->call(original);
  CHECK(!timed_out.ok());
  CHECK(timed_out.error.code == domain::ErrorCode::kTimeout);

  transport::ZmqControlServerConfig config;
  config.endpoint = endpoint;
  config.poll_interval = Milliseconds{5};
  config.send_timeout = Milliseconds{200};
  config.bind_wait_budget = Milliseconds{1000};
  fixture.server.reset(new transport::ZmqControlServer(fixture.gateway, config));
  CHECK(fixture.server->start().ok());

  const auto replayed = fixture.client->call(original);
  CHECK(replayed.ok());
  CHECK(replayed.value->result.ok());
  CHECK(replayed.value->replayed);
}

// 保护不变量：exit 属于按预算等待清理的终态操作；推理仍被闸门阻塞时，exit 不会等到
// 推理结束才回复，而是按请求预算返回结构化超时结果，并把“未退出”如实交给客户端。
void TestExitRespectsRequestBudgetWhileInferenceRuns() {
  Fixture fixture;
  fixture.factory.hold_run();
  CHECK(fixture.StartServerAndClient().ok());
  const auto started = fixture.client->call(Request("start-exit", "start"));
  CHECK(started.ok());

  const auto exited =
      fixture.client->call(Request("exit-budget", "exit", Milliseconds{250}));
  CHECK(exited.ok());
  CHECK(!exited.value->result.ok());
  CHECK(exited.value->result.error.code == domain::ErrorCode::kTimeout);

  fixture.factory.release_run();
  CHECK(fixture.WaitForIdle());
}

// 保护不变量：容量、水位和预算不是实现常量，而是显式配置；非法值在建立 socket 之前
// 被结构化拒绝，不会被静默夹成默认值后继续运行。
void TestTransportConfigValidation() {
  transport::ZmqControlServerConfig server;
  server.endpoint.clear();
  CHECK(!transport::validate_zmq_control_server_config(server).ok());
  server = transport::ZmqControlServerConfig{};
  server.receive_high_water_mark = 0;
  CHECK(!transport::validate_zmq_control_server_config(server).ok());
  server = transport::ZmqControlServerConfig{};
  server.send_high_water_mark = 0;
  CHECK(!transport::validate_zmq_control_server_config(server).ok());
  server = transport::ZmqControlServerConfig{};
  server.poll_interval = Milliseconds{0};
  CHECK(!transport::validate_zmq_control_server_config(server).ok());
  server = transport::ZmqControlServerConfig{};
  server.send_timeout = Milliseconds{0};
  CHECK(!transport::validate_zmq_control_server_config(server).ok());
  server = transport::ZmqControlServerConfig{};
  server.bind_wait_budget = Milliseconds{0};
  CHECK(!transport::validate_zmq_control_server_config(server).ok());
  server = transport::ZmqControlServerConfig{};
  server.max_response_bytes = 0;
  CHECK(!transport::validate_zmq_control_server_config(server).ok());

  transport::ZmqControlClientConfig client;
  client.send_timeout = Milliseconds{0};
  CHECK(!transport::validate_zmq_control_client_config(client).ok());
  client = transport::ZmqControlClientConfig{};
  client.receive_high_water_mark = 0;
  CHECK(!transport::validate_zmq_control_client_config(client).ok());
  client = transport::ZmqControlClientConfig{};
  CHECK(transport::validate_zmq_control_client_config(client).ok());
}

}  // namespace

int main() {
  TestControlRemainsReachableWhileSessionRuns();
  TestDuplicateRequestDoesNotStartASecondSession();
  TestRepeatedStartStopIsClean();
  TestClientTimeoutIsStructured();
  TestStopWhileSessionIsRunningReleasesThread();
  TestClientReconnectsAfterServerRestartAndReplaysRequest();
  TestExitRespectsRequestBudgetWhileInferenceRuns();
  TestTransportConfigValidation();
}

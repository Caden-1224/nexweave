#include "../test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "child_process.hpp"
#include "gateway.hpp"
#include "zmq_gateway_route.hpp"

using namespace nexweave;
using namespace std::chrono_literals;

#ifndef NEXWEAVE_REMOTE_GATEWAY_FIXTURE
#error "测试需要 NEXWEAVE_REMOTE_GATEWAY_FIXTURE 指向远端网关夹具"
#endif

namespace {

std::string MakeTempPath() {
  char path[] = "/tmp/nexweave-route-XXXXXX";
  const int fd = ::mkstemp(path);
  if (fd >= 0) {
    ::close(fd);
  }
  std::error_code error;
  std::filesystem::remove(path, error);
  return path;
}

std::string ReadEndpoint(const std::string& path) {
  const auto deadline = std::chrono::steady_clock::now() + 2000ms;
  while (std::chrono::steady_clock::now() < deadline) {
    std::ifstream file(path);
    std::string endpoint;
    if (file.is_open()) {
      std::getline(file, endpoint);
      if (!endpoint.empty()) {
        return endpoint;
      }
    }
    std::this_thread::sleep_for(2ms);
  }
  return std::string();
}

runtime::ChildProcessSpec RemoteSpec(const std::string& endpoint_file) {
  runtime::ChildProcessSpec spec;
  spec.executable = NEXWEAVE_REMOTE_GATEWAY_FIXTURE;
  spec.arguments = {"--endpoint-file", endpoint_file};
  return spec;
}

runtime::ChildProcessConfig TightConfig() {
  runtime::ChildProcessConfig config;
  config.start_wait_budget = 1500ms;
  config.stop_wait_budget = 300ms;
  config.kill_wait_budget = 500ms;
  config.poll_interval = 2ms;
  return config;
}

protocol::ControlRequest MakeRequest(std::string request_id, std::string operation,
                                     std::string work_id = {},
                                     std::string session_id = {},
                                     std::chrono::milliseconds deadline = 1000ms) {
  protocol::ControlRequest request;
  request.request_id = std::move(request_id);
  request.operation = std::move(operation);
  request.work_id = std::move(work_id);
  request.session_id = std::move(session_id);
  request.deadline = deadline;
  return request;
}

domain::Result<protocol::ControlResponse> Exchange(gateway::Gateway& entry,
                                                  gateway::ConnectionId connection,
                                                  const protocol::ControlRequest& request) {
  const auto encoded = protocol::encode_request(request);
  if (!encoded.ok()) {
    return domain::Result<protocol::ControlResponse>::failure(encoded.error.code,
                                                              encoded.error.message);
  }
  std::string frame = *encoded.value;
  frame.push_back('\n');
  // feed() 的返回值只表示连接之后是否仍打开；携带 exit 的请求会先处理再关闭，因此不能把
  // false 当成“没有响应”。flush() 才是“还有没有答复可取”的判据。
  (void)entry.feed(connection, frame);
  std::string response_bytes;
  entry.flush(connection, response_bytes, 64U * 1024U);
  const std::size_t newline = response_bytes.find('\n');
  if (newline != std::string::npos) {
    response_bytes.resize(newline);
  }
  return protocol::decode_response(response_bytes);
}

std::string FactValue(const std::string& message, const std::string& key) {
  const std::string needle = key + "=";
  const std::size_t begin = message.find(needle);
  if (begin == std::string::npos) {
    return std::string();
  }
  const std::size_t value_begin = begin + needle.size();
  const std::size_t value_end = message.find(' ', value_begin);
  if (value_end == std::string::npos) {
    return message.substr(value_begin);
  }
  return message.substr(value_begin, value_end - value_begin);
}

void TestRemoteControlRouteEndToEnd() {
  const std::string endpoint_file = MakeTempPath();
  runtime::ChildProcess fixture(TightConfig());
  const auto fixture_started = fixture.start(RemoteSpec(endpoint_file));
  CHECK(fixture_started.ok());
  const runtime::ChildProcessIdentity fixture_identity = *fixture_started.value;

  const std::string endpoint = ReadEndpoint(endpoint_file);
  CHECK(!endpoint.empty());

  transport::ZmqControlRoute route(endpoint);
  CHECK(route.connect().ok());

  gateway::Gateway entry(route);
  const gateway::ConnectionId connection = entry.open_connection();
  CHECK(connection != gateway::kInvalidConnectionId);

  const auto start = Exchange(entry, connection, MakeRequest("r-start", "start", "w-1", "s-1"));
  CHECK(start.ok());
  CHECK(start.value->result.ok());
  const std::string work_id = FactValue(start.value->result.error.message, "work_id");
  const std::string session_id = FactValue(start.value->result.error.message, "session_id");
  CHECK(work_id == "w-1");
  CHECK(session_id == "s-1");

  // 重复的 start 在事件终态到达之前必须保留“执行中”的幂等语义，不能启动第二次远端操作。
  const auto duplicate = Exchange(entry, connection,
                                  MakeRequest("r-start", "start", "w-1", "s-1"));
  CHECK(duplicate.ok());
  CHECK(!duplicate.value->result.ok());
  CHECK(duplicate.value->result.error.code == domain::ErrorCode::kAlreadyCompleted);

  // 远端拥有者停在 run() 上；此时 query 仍必须被独立处理并看到 active。
  const auto active = Exchange(entry, connection, MakeRequest("r-query-1", "query"));
  CHECK(active.ok());
  CHECK(active.value->result.ok());
  CHECK(FactValue(active.value->result.error.message, "state") == "active");

  const auto cancel =
      Exchange(entry, connection, MakeRequest("r-cancel", "cancel", "w-1", "s-1"));
  CHECK(cancel.ok());
  CHECK(cancel.value->result.ok());
  CHECK(FactValue(cancel.value->result.error.message, "accepted") == "1");

  bool idle = false;
  for (int attempt = 0; attempt < 100 && !idle; ++attempt) {
    const auto query = Exchange(entry, connection, MakeRequest("r-query-" + std::to_string(attempt + 2), "query"));
    CHECK(query.ok());
    CHECK(query.value->result.ok());
    idle = FactValue(query.value->result.error.message, "state") == "idle";
    if (!idle) {
      std::this_thread::sleep_for(5ms);
    }
  }
  CHECK(idle);

  const auto exit = Exchange(entry, connection, MakeRequest("r-exit", "exit"));
  CHECK(exit.ok());
  CHECK(exit.value->result.ok());
  CHECK(!entry.connection_open(connection));

  // 远端进程退出后，同一路由上的新请求必须得到结构化失败而不是挂起或伪成功。
  CHECK(fixture.stop(fixture_identity, 1000ms).ok());
  gateway::Gateway entry_after_exit(route);
  const gateway::ConnectionId after_exit = entry_after_exit.open_connection();
  CHECK(after_exit != gateway::kInvalidConnectionId);
  const auto unavailable =
      Exchange(entry_after_exit, after_exit, MakeRequest("r-after-exit", "query", {}, {}, 200ms));
  CHECK(unavailable.ok());
  CHECK(!unavailable.value->result.ok());
  CHECK(unavailable.value->result.error.code == domain::ErrorCode::kTimeout ||
        unavailable.value->result.error.code == domain::ErrorCode::kBackendFailure);

  std::filesystem::remove(endpoint_file);
}

void TestUnavailableEndpointReturnsStructuredError() {
  transport::ZmqControlRoute route("tcp://127.0.0.1:1");
  CHECK(route.connect().ok());
  gateway::Gateway entry(route);
  const gateway::ConnectionId connection = entry.open_connection();
  const auto response =
      Exchange(entry, connection, MakeRequest("r-unavailable", "query", {}, {}, 150ms));
  CHECK(response.ok());
  CHECK(!response.value->result.ok());
  CHECK(response.value->result.error.code == domain::ErrorCode::kTimeout ||
        response.value->result.error.code == domain::ErrorCode::kBackendFailure);
}

// 事件能力用一个可控假路由验证：真实 ZeroMQ 控制适配器当前只搬控制响应，但 Gateway 的
// 路由模式必须继续支持 data event 队列，后续事件传输接上后不需要再改外部协议。
class FakeEventRoute final : public gateway::IControlRoute {
 public:
  domain::Result<protocol::ControlResponse> call(
      const protocol::ControlRequest& request) override {
    protocol::ControlResponse response;
    response.request_id = request.request_id;
    response.result = domain::OperationResult::success();
    if (request.operation == "start") {
      response.result.error.message = "work_id=w-1 session_id=s-evt session_sequence=1";
    } else {
      response.result.error.message = "state=idle";
    }
    return domain::Result<protocol::ControlResponse>::success(std::move(response));
  }

  std::size_t poll_events(gateway::ControlRouteOwner owner,
                          std::vector<protocol::DataEvent>& out,
                          std::size_t max_events) override {
    (void)owner;
    if (emitted_) {
      return 0;
    }
    emitted_ = true;
    protocol::DataEvent token;
    token.type = protocol::DataEventType::kToken;
    token.text = "hello";
    out.push_back(token);
    if (out.size() >= max_events) {
      return 1;
    }
    protocol::DataEvent done;
    done.type = protocol::DataEventType::kDone;
    done.end = true;
    out.push_back(done);
    return 2;
  }

 private:
  bool emitted_ = false;
};

void TestRouteModePreservesEventQueue() {
  FakeEventRoute route;
  gateway::Gateway entry(route);
  const gateway::ConnectionId connection = entry.open_connection();
  const auto started = Exchange(entry, connection, MakeRequest("r-evt", "start", "w-1", "s-evt"));
  CHECK(started.ok());
  CHECK(started.value->result.ok());

  CHECK(entry.deliver_settled() == 2);
  std::string bytes;
  entry.flush(connection, bytes, 64U * 1024U);
  std::vector<std::string> lines;
  std::size_t position = 0;
  while (position < bytes.size()) {
    const std::size_t newline = bytes.find('\n', position);
    if (newline == std::string::npos) {
      break;
    }
    lines.push_back(bytes.substr(position, newline - position));
    position = newline + 1;
  }
  CHECK(lines.size() == 2);
  const auto token = protocol::decode_event_metadata(lines[0]);
  const auto done = protocol::decode_event_metadata(lines[1]);
  CHECK(token.ok());
  CHECK(done.ok());
  CHECK(token.value->request_id == "r-evt");
  CHECK(token.value->session_id == "s-evt");
  CHECK(token.value->type == protocol::DataEventType::kToken);
  CHECK(done.value->end);
}

}  // namespace

int main() {
  TestRemoteControlRouteEndToEnd();
  TestUnavailableEndpointReturnsStructuredError();
  TestRouteModePreservesEventQueue();
  return 0;
}

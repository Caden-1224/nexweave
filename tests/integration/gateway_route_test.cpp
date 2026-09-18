#include "../test_support.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "child_process.hpp"
#include "gateway.hpp"
#include "remote_session_route.hpp"
#include "zmq_gateway_route.hpp"

using namespace nexweave;
using namespace std::chrono_literals;

#ifndef NEXWEAVE_REMOTE_GATEWAY_FIXTURE
#error "测试需要 NEXWEAVE_REMOTE_GATEWAY_FIXTURE 指向远端网关夹具"
#endif
#ifndef NEXWEAVE_REMOTE_SESSION_FIXTURE
#error "测试需要 NEXWEAVE_REMOTE_SESSION_FIXTURE 指向远端 Session 夹具"
#endif
#ifndef NEXWEAVE_REMOTE_SCRIPTED_FIXTURE
#error "测试需要 NEXWEAVE_REMOTE_SCRIPTED_FIXTURE 指向脚本夹具"
#endif

namespace {

// 可控音频源：测试线程向队列推送帧，路由输入线程阻塞读取；EOF 由 Finish 显式给出。
// 这样测试可以在输入结束前观察远端是否已经消费前序音频并回传终态。
struct ScriptedSourceState {
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<domain::AudioFrame> frames;
  bool ended = false;
  bool cancelled = false;
};

class ScriptedAudioSource final : public capability::IAudioSource {
 public:
  explicit ScriptedAudioSource(std::shared_ptr<ScriptedSourceState> state)
      : state_(std::move(state)) {}

  domain::OperationResult open() override {
    return domain::OperationResult::success();
  }

  domain::Result<domain::AudioFrame> read() override {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->condition.wait(lock, [this] {
      return state_->cancelled || state_->ended || !state_->frames.empty();
    });
    if (state_->cancelled) {
      return domain::Result<domain::AudioFrame>::failure(domain::ErrorCode::kCancelled,
                                                         "输入源已取消");
    }
    if (!state_->frames.empty()) {
      domain::AudioFrame frame = std::move(state_->frames.front());
      state_->frames.pop_front();
      return domain::Result<domain::AudioFrame>::success(std::move(frame));
    }
    return domain::Result<domain::AudioFrame>::failure(domain::ErrorCode::kAlreadyCompleted,
                                                       "输入源已结束");
  }

  domain::OperationResult cancel() noexcept override {
    {
      const std::lock_guard<std::mutex> lock(state_->mutex);
      state_->cancelled = true;
    }
    state_->condition.notify_all();
    return domain::OperationResult::success();
  }

  domain::OperationResult close() noexcept override {
    {
      const std::lock_guard<std::mutex> lock(state_->mutex);
      state_->ended = true;
    }
    state_->condition.notify_all();
    return domain::OperationResult::success();
  }

 private:
  std::shared_ptr<ScriptedSourceState> state_;
};

void PushSourceFrame(const std::shared_ptr<ScriptedSourceState>& state,
                     std::int16_t value) {
  const auto frame = domain::AudioFrame::from_samples(
      std::vector<std::int16_t>(domain::kAudioFrameSamples, value));
  CHECK(frame.ok());
  {
    const std::lock_guard<std::mutex> lock(state->mutex);
    state->frames.push_back(frame.value);
  }
  state->condition.notify_all();
}

void FinishSource(const std::shared_ptr<ScriptedSourceState>& state) {
  {
    const std::lock_guard<std::mutex> lock(state->mutex);
    state->ended = true;
  }
  state->condition.notify_all();
}

std::vector<protocol::DataEvent> CollectDataEvents(gateway::Gateway& entry,
                                                   gateway::ConnectionId connection,
                                                   std::chrono::milliseconds budget) {
  std::vector<protocol::DataEvent> events;
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    (void)entry.deliver_settled();
    std::string bytes;
    entry.flush(connection, bytes, 64U * 1024U);
    std::size_t position = 0;
    bool terminal = false;
    while (position < bytes.size()) {
      const std::size_t newline = bytes.find('\n', position);
      if (newline == std::string::npos) {
        break;
      }
      const std::string line = bytes.substr(position, newline - position);
      position = newline + 1;
      const auto decoded = protocol::decode_event_metadata(line);
      if (!decoded.ok()) {
        continue;
      }
      if (decoded.value->end) {
        terminal = true;
      }
      events.push_back(*decoded.value);
    }
    if (terminal) {
      return events;
    }
    std::this_thread::sleep_for(1ms);
  }
  return events;
}


std::vector<protocol::DataEvent> CollectRouteEvents(
    transport::RemoteSessionRoute& route,
    std::chrono::milliseconds budget) {
  std::vector<protocol::DataEvent> events;
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    std::vector<protocol::DataEvent> batch;
    const std::size_t count = route.poll_events(1, batch, 64);
    if (count == 0) {
      std::this_thread::sleep_for(1ms);
      continue;
    }
    bool terminal = false;
    for (protocol::DataEvent& event : batch) {
      if (event.end) {
        terminal = true;
      }
      events.push_back(std::move(event));
    }
    if (terminal) {
      return events;
    }
  }
  return events;
}

bool HasTerminal(const std::vector<protocol::DataEvent>& events,
                 protocol::DataEventType type, domain::ErrorCode error_code) {
  for (const protocol::DataEvent& event : events) {
    if (event.end && event.type == type && event.error_code == error_code) {
      return true;
    }
  }
  return false;
}

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
  // false 当成“没有响应”。路由模式是异步转发：在请求 deadline 内反复 flush，由 flush
  // 驱动完成队列落地，直到取到一条响应或者预算耗尽。
  (void)entry.feed(connection, frame);
  const std::chrono::milliseconds budget =
      request.deadline > 10ms ? request.deadline : 10ms;
  const auto deadline = std::chrono::steady_clock::now() + budget + 200ms;
  std::string response_bytes;
  while (std::chrono::steady_clock::now() < deadline) {
    response_bytes.clear();
    entry.flush(connection, response_bytes, 64U * 1024U);
    if (!response_bytes.empty()) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  const std::size_t newline = response_bytes.find('\n');
  if (newline != std::string::npos) {
    response_bytes.resize(newline);
  }
  return protocol::decode_response(response_bytes);
}

void FeedRequest(gateway::Gateway& entry, gateway::ConnectionId connection,
                 const protocol::ControlRequest& request) {
  const auto encoded = protocol::encode_request(request);
  CHECK(encoded.ok());
  std::string frame = *encoded.value;
  frame.push_back('\n');
  (void)entry.feed(connection, frame);
}

std::string CollectResponse(gateway::Gateway& entry, gateway::ConnectionId connection,
                            std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    std::string bytes;
    entry.flush(connection, bytes, 64U * 1024U);
    if (!bytes.empty()) {
      const std::size_t newline = bytes.find('\n');
      return newline == std::string::npos ? bytes : bytes.substr(0, newline);
    }
    std::this_thread::sleep_for(1ms);
  }
  return std::string();
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
      Exchange(entry_after_exit, after_exit, MakeRequest("r-after-exit", "query", {}, {}, 500ms));
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
      Exchange(entry, connection, MakeRequest("r-unavailable", "query", {}, {}, 500ms));
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

// 慢转发测试路由：start 故意占用一个工作线程，query 立即可返回。如果 Gateway 仍同步调用
// 路由，后续 query 会被 start 的 300 ms 阻塞并在 150 ms 预算内超时。
class SlowRoute final : public gateway::IControlRoute {
 public:
  domain::Result<protocol::ControlResponse> call(
      const protocol::ControlRequest& request) override {
    if (request.operation == "start") {
      std::this_thread::sleep_for(500ms);
    }
    protocol::ControlResponse response;
    response.request_id = request.request_id;
    response.result = domain::OperationResult::success();
    response.result.error.message =
        request.operation == "start" ? "work_id=w-1 session_id=s-1 session_sequence=1"
                                     : "state=idle";
    return domain::Result<protocol::ControlResponse>::success(std::move(response));
  }

  std::size_t poll_events(gateway::ControlRouteOwner owner,
                          std::vector<protocol::DataEvent>& out,
                          std::size_t max_events) override {
    (void)owner;
    (void)out;
    (void)max_events;
    return 0;
  }
};

void TestSlowForwardDoesNotBlockOtherControls() {
  SlowRoute route;
  gateway::GatewayConfig config;
  config.max_route_workers = 4;
  gateway::Gateway entry(route, config);
  const gateway::ConnectionId slow_connection = entry.open_connection();
  const gateway::ConnectionId fast_connection = entry.open_connection();
  CHECK(slow_connection != gateway::kInvalidConnectionId);
  CHECK(fast_connection != gateway::kInvalidConnectionId);

  FeedRequest(entry, slow_connection, MakeRequest("r-slow-start", "start", "w-1", "s-1", 1000ms));
  const auto query = Exchange(entry, fast_connection,
                              MakeRequest("r-fast-query", "query", {}, {}, 150ms));
  CHECK(query.ok());
  CHECK(query.value->result.ok());
  CHECK(FactValue(query.value->result.error.message, "state") == "idle");

  const std::string slow_response = CollectResponse(entry, slow_connection, 1000ms);
  const auto started = protocol::decode_response(slow_response);
  CHECK(started.ok());
  CHECK(started.value->result.ok());
}

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

void TestRemoteSessionRouteEndToEnd() {
  auto source_state = std::make_shared<ScriptedSourceState>();
  transport::RemoteSessionRouteConfig config;
  config.proxy.child.executable = NEXWEAVE_REMOTE_SESSION_FIXTURE;
  config.proxy.child.expect_ready_signal = true;
  config.proxy.endpoint_file = MakeTempPath();
  config.proxy.ready_timeout = 5000ms;
  config.proxy.pump_interval = 5ms;
  config.proxy.terminal_retention = 30s;
  const std::string trace_file = MakeTempPath();
  config.proxy.child.environment = {
      "NEXWEAVE_REMOTE_SESSION_TRACE_FILE=" + trace_file};
  config.input_factory = [source_state]() -> std::unique_ptr<capability::IAudioSource> {
    return std::make_unique<ScriptedAudioSource>(source_state);
  };
  config.stream_id = "remote-session";

  transport::RemoteSessionRoute route(config);
  gateway::Gateway entry(route);
  const gateway::ConnectionId first = entry.open_connection();
  const gateway::ConnectionId second = entry.open_connection();
  CHECK(first != gateway::kInvalidConnectionId);
  CHECK(second != gateway::kInvalidConnectionId);

  const auto started = Exchange(
      entry, first, MakeRequest("r-rs-start", "start", "w-1", "s-rs", 5000ms));
  CHECK(started.ok());
  CHECK(started.value->result.ok());

  // 第二个客户端在首个会话终态前不能创建并行会话；忙碌拒绝必须来自路由本身。
  const auto busy = Exchange(
      entry, second, MakeRequest("r-rs-busy", "start", "w-2", "s-rs-2", 1000ms));
  CHECK(busy.ok());
  CHECK(!busy.value->result.ok());
  CHECK(busy.value->result.error.code == domain::ErrorCode::kBusy);

  // 输入尚未结束时，远端应先完成一段说话并回传文本/PCM/终态。
  PushSourceFrame(source_state, 1000);
  PushSourceFrame(source_state, 1000);
  PushSourceFrame(source_state, 0);
  PushSourceFrame(source_state, 0);
  const auto events = CollectDataEvents(entry, first, 5000ms);
  bool has_text = false;
  bool has_pcm = false;
  bool has_terminal = false;
  for (const protocol::DataEvent& event : events) {
    if (event.type == protocol::DataEventType::kFinal && !event.text.empty()) {
      has_text = true;
    }
    if (event.type == protocol::DataEventType::kPcm) {
      has_pcm = true;
    }
    if (event.end) {
      has_terminal = true;
    }
  }
  CHECK(has_text);
  CHECK(has_pcm);
  CHECK(has_terminal);
  {
    const std::lock_guard<std::mutex> lock(source_state->mutex);
    CHECK(!source_state->ended);
  }

  FinishSource(source_state);
  const auto exited = Exchange(
      entry, first, MakeRequest("r-rs-exit", "exit", "w-1", "s-rs", 2000ms));
  CHECK(exited.ok());
  CHECK(exited.value->result.ok());
  CHECK(entry.status().closed);

  // 独立进程中的 SessionApp 也必须保持“播放开始早于文本定稿”的单进程语义；
  // 这里读取远端夹具落盘的标记顺序，而不是用客户端事件到达顺序冒充。
  std::vector<int> markers;
  {
    std::ifstream trace(trace_file);
    int marker = 0;
    while (trace >> marker) {
      markers.push_back(marker);
    }
  }
  const auto playback_started = std::find(
      markers.begin(), markers.end(),
      static_cast<int>(runtime::ActivityMarker::kPlaybackStarted));
  const auto generation_done = std::find(
      markers.begin(), markers.end(),
      static_cast<int>(runtime::ActivityMarker::kGenerationDone));
  CHECK(playback_started != markers.end());
  CHECK(generation_done != markers.end());
  CHECK(playback_started < generation_done);

  // 夹具退出后回收本用例创建的临时文件；端点文件与 trace 文件都不能留给下一次运行。
  std::error_code remove_error;
  std::filesystem::remove(trace_file, remove_error);
  std::filesystem::remove(config.proxy.endpoint_file, remove_error);
}

// 保护不变量：取消可以早于输入线程建立远端输入流；此时第一次 queue_cancel_stream()
// 还没有活动流可取消，路由必须在流建立后补发取消，否则远端会永远等待输入。随后同一路由
// 上的下一次 start 必须能建立新会话，不能被旧清理路径删除。
void TestRemoteCancelBeforeFirstFrameAndRestart() {
  const std::string endpoint_file = MakeTempPath();
  const auto first_state = std::make_shared<ScriptedSourceState>();
  const auto second_state = std::make_shared<ScriptedSourceState>();
  auto pending_states =
      std::make_shared<std::deque<std::shared_ptr<ScriptedSourceState>>>();
  pending_states->push_back(first_state);
  pending_states->push_back(second_state);

  transport::RemoteSessionRouteConfig config;
  config.proxy.child.executable = NEXWEAVE_REMOTE_SESSION_FIXTURE;
  config.proxy.child.expect_ready_signal = true;
  config.proxy.endpoint_file = endpoint_file;
  config.proxy.ready_timeout = 5000ms;
  config.proxy.pump_interval = 5ms;
  config.cancel_timeout = 5000ms;
  config.input_factory = [pending_states]() -> std::unique_ptr<capability::IAudioSource> {
    if (pending_states->empty()) {
      return {};
    }
    const std::shared_ptr<ScriptedSourceState> state = pending_states->front();
    pending_states->pop_front();
    return std::make_unique<ScriptedAudioSource>(state);
  };
  config.stream_id = "remote-session";

  transport::RemoteSessionRoute route(config);
  const auto started = route.call(
      MakeRequest("r-cancel-before-frame", "start", "w-1", "s-1", 3000ms));
  CHECK(started.ok());
  CHECK(started.value->result.ok());

  // 立即取消，输入线程可能尚未执行 queue_start_stream()，用来覆盖取消早到竞态。
  const auto cancelled = route.call(
      MakeRequest("r-cancel-before-frame-action", "cancel", "w-1", "s-1", 5000ms));
  CHECK(cancelled.ok());
  CHECK(cancelled.value->result.ok());

  const auto first_events = CollectRouteEvents(route, 8s);
  CHECK(HasTerminal(first_events, protocol::DataEventType::kError,
                    domain::ErrorCode::kCancelled));
  CHECK(route.stats().cancel_requests == 1);
  CHECK(route.stats().terminal_events_delivered == 1);
  CHECK(route.stats().last_cancel_accept_to_terminal.count() > 0);

  // 旧会话收敛后必须能重新建立新会话；新会话输出不能因为旧清理而被丢弃。
  const auto restarted = route.call(
      MakeRequest("r-cancel-before-frame-restart", "start", "w-2", "s-2", 5000ms));
  CHECK(restarted.ok());
  CHECK(restarted.value->result.ok());

  PushSourceFrame(second_state, 1000);
  PushSourceFrame(second_state, 1000);
  PushSourceFrame(second_state, 0);
  PushSourceFrame(second_state, 0);
  const auto second_events = CollectRouteEvents(route, 5s);
  CHECK(HasTerminal(second_events, protocol::DataEventType::kDone,
                    domain::ErrorCode::kNone));
  std::size_t terminal_count = 0;
  for (const protocol::DataEvent& event : second_events) {
    if (event.end) {
      ++terminal_count;
    }
  }
  CHECK(terminal_count == 1);
  CHECK(route.stats().starts_accepted == 2);

  const auto exited = route.call(MakeRequest("r-cancel-before-frame-exit", "exit"));
  CHECK(exited.ok());
  CHECK(exited.value->result.ok());

  std::error_code remove_error;
  std::filesystem::remove(endpoint_file, remove_error);
}

// 保护不变量：取消预算到期而远端没有产生终态时，poll_events() 必须交付唯一的结构化
// kTimeout 终态并请求停止；不能把取消响应 ok 当成后端已经停止，也不能无限等待。
void TestRemoteCancelTimeoutIsStructured() {
  const std::string endpoint_file = MakeTempPath();
  transport::RemoteSessionRouteConfig config;
  config.proxy.child.executable = NEXWEAVE_REMOTE_SCRIPTED_FIXTURE;
  config.proxy.child.expect_ready_signal = true;
  config.proxy.endpoint_file = endpoint_file;
  config.proxy.ready_timeout = 5000ms;
  config.proxy.pump_interval = 5ms;
  config.cancel_timeout = 50ms;
  config.input_factory = []() -> std::unique_ptr<capability::IAudioSource> {
    return std::make_unique<ScriptedAudioSource>(std::make_shared<ScriptedSourceState>());
  };
  config.stream_id = "remote-session";
  config.proxy.child.environment = {
      "NEXWEAVE_SCRIPTED_MODE=block",
      "NEXWEAVE_SCRIPTED_BLOCK_MS=2000",
  };

  transport::RemoteSessionRoute route(config);
  const auto started = route.call(
      MakeRequest("r-cancel-timeout-start", "start", "w-1", "s-1", 3000ms));
  CHECK(started.ok());
  CHECK(started.value->result.ok());

  const auto cancelled = route.call(
      MakeRequest("r-cancel-timeout-action", "cancel", "w-1", "s-1", 1000ms));
  CHECK(cancelled.ok());
  CHECK(cancelled.value->result.ok());

  const auto events = CollectRouteEvents(route, 2s);
  CHECK(HasTerminal(events, protocol::DataEventType::kError,
                    domain::ErrorCode::kTimeout));
  CHECK(route.stats().cancel_timeouts == 1);
  CHECK(route.stats().terminal_events_delivered == 1);
  CHECK(route.stats().last_cancel_accept_to_terminal.count() > 0);

  const auto exited = route.call(MakeRequest("r-cancel-timeout-exit", "exit"));
  CHECK(exited.ok());
  CHECK(exited.value->result.ok());

  std::error_code remove_error;
  std::filesystem::remove(endpoint_file, remove_error);
}

// 保护不变量：终态之后仍然到达的旧事件不能进入上层，也不能污染同一路由上的下一次
// start；旧清理只作用于旧代理和旧队列，新代理的输出必须完整交付。
void TestStaleEventsDoNotLeakIntoNextStart() {
  const std::string endpoint_file = MakeTempPath();
  transport::RemoteSessionRouteConfig config;
  config.proxy.child.executable = NEXWEAVE_REMOTE_SCRIPTED_FIXTURE;
  config.proxy.child.expect_ready_signal = true;
  config.proxy.endpoint_file = endpoint_file;
  config.proxy.ready_timeout = 5000ms;
  config.proxy.pump_interval = 5ms;
  config.cancel_timeout = 500ms;
  config.input_factory = []() -> std::unique_ptr<capability::IAudioSource> {
    return std::make_unique<ScriptedAudioSource>(std::make_shared<ScriptedSourceState>());
  };
  config.stream_id = "remote-session";
  config.proxy.child.environment = {"NEXWEAVE_SCRIPTED_MODE=stale"};

  transport::RemoteSessionRoute route(config);
  const auto started = route.call(
      MakeRequest("r-stale-start", "start", "w-1", "s-1", 3000ms));
  CHECK(started.ok());
  CHECK(started.value->result.ok());

  const auto first_events = CollectRouteEvents(route, 3s);
  CHECK(HasTerminal(first_events, protocol::DataEventType::kDone,
                    domain::ErrorCode::kNone));
  std::size_t first_terminals = 0;
  for (const protocol::DataEvent& event : first_events) {
    if (event.end) {
      ++first_terminals;
    }
    CHECK(event.text != "late-new-key");
  }
  CHECK(first_terminals == 1);

  // 终态之后再次 poll 不能交付旧键的新轮次事件。
  std::vector<protocol::DataEvent> after_terminal;
  CHECK(route.poll_events(1, after_terminal, 64) == 0);
  CHECK(after_terminal.empty());

  // 新 start 必须重新建立终态水位并交付自己的唯一终态。
  const auto restarted = route.call(
      MakeRequest("r-stale-restart", "start", "w-2", "s-2", 3000ms));
  CHECK(restarted.ok());
  CHECK(restarted.value->result.ok());
  const auto second_events = CollectRouteEvents(route, 3s);
  CHECK(HasTerminal(second_events, protocol::DataEventType::kDone,
                    domain::ErrorCode::kNone));
  std::size_t second_terminals = 0;
  for (const protocol::DataEvent& event : second_events) {
    if (event.end) {
      ++second_terminals;
    }
    CHECK(event.text != "late-new-key");
  }
  CHECK(second_terminals == 1);

  const auto exited = route.call(MakeRequest("r-stale-exit", "exit"));
  CHECK(exited.ok());
  CHECK(exited.value->result.ok());

  std::error_code remove_error;
  std::filesystem::remove(endpoint_file, remove_error);
}

}  // namespace

int main() {
  TestRemoteControlRouteEndToEnd();
  TestRemoteSessionRouteEndToEnd();
  TestUnavailableEndpointReturnsStructuredError();
  TestSlowForwardDoesNotBlockOtherControls();
  TestRouteModePreservesEventQueue();
  TestRemoteCancelBeforeFirstFrameAndRestart();
  TestRemoteCancelTimeoutIsStructured();
  TestStaleEventsDoNotLeakIntoNextStart();
  return 0;
}

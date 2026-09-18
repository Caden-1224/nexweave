#include "remote_session_route.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace nexweave::transport {
namespace {

using domain::ErrorCode;
using domain::OperationResult;
using protocol::ControlRequest;
using protocol::ControlResponse;
using protocol::DataEvent;

std::string FactLine(const std::string& state, const std::string& work_id,
                     const std::string& session_id, std::uint64_t generation,
                     bool input_finished, bool terminal_seen) {
  std::string message = "state=" + state;
  if (!work_id.empty()) {
    message += " work_id=" + work_id;
  }
  if (!session_id.empty()) {
    message += " session_id=" + session_id;
  }
  message += " generation=" + std::to_string(generation);
  message += " input_finished=";
  message += input_finished ? "true" : "false";
  message += " terminal_seen=";
  message += terminal_seen ? "true" : "false";
  return message;
}

ControlResponse MakeResponse(const ControlRequest& request,
                             const OperationResult& result) {
  ControlResponse response;
  response.request_id = request.request_id;
  response.result = result;
  return response;
}

const char* StateName(std::uint8_t state) noexcept {
  switch (state) {
    case 0:
      return "idle";
    case 1:
      return "active";
    case 2:
      return "completed";
    case 3:
      return "stopping";
    case 4:
      return "unavailable";
    default:
      return "unknown";
  }
}

}  // namespace

domain::OperationResult validate_remote_session_route_config(
    const RemoteSessionRouteConfig& config) {
  const auto proxy_valid = validate_remote_session_proxy_config(config.proxy);
  if (!proxy_valid.ok()) {
    return proxy_valid;
  }
  if (!config.input_factory) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "输入源工厂不能为空");
  }
  if (config.stream_id.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "输入流标识不能为空");
  }
  if (config.frame_interval.count() < 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "帧间隔不能为负");
  }
  if (config.cancel_timeout.count() <= 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "取消收敛预算必须为正");
  }
  return OperationResult::success();
}

struct RemoteSessionRoute::Impl {
  enum class State : std::uint8_t {
    kIdle = 0,
    kActive = 1,
    kCompleted = 2,
    kStopping = 3,
    kUnavailable = 4
  };

  explicit Impl(RemoteSessionRouteConfig config_value)
      : config(std::move(config_value)), proxy(config.proxy) {}

  RemoteSessionRouteConfig config;
  RemoteSessionProxy proxy;
  mutable std::mutex mutex;
  mutable std::mutex error_mutex;
  std::thread input_thread;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> input_finished{false};
  std::atomic<bool> terminal_seen{false};
  std::atomic<capability::IAudioSource*> source{nullptr};
  std::uint64_t generation = 0;
  std::string request_id;
  std::string work_id;
  std::string session_id;
  State state = State::kIdle;
  domain::Error last_error{};
  // 取消计时与终态过滤。cancel_accepted_at 在第一次取消受理时写入；取消预算到期且
  // 还没有终态时，poll_events() 合成一条 kTimeout 终态并请求代理停止。
  std::chrono::steady_clock::time_point cancel_accepted_at{};
  std::chrono::steady_clock::time_point cancel_deadline{};
  bool cancellation_pending = false;
  RemoteSessionRouteStats stats{};

  domain::Result<ControlResponse> HandleStart(const ControlRequest& request);
  domain::Result<ControlResponse> HandleQuery(const ControlRequest& request);
  domain::Result<ControlResponse> HandleCancel(const ControlRequest& request);
  domain::Result<ControlResponse> HandleExit(const ControlRequest& request);
  void RunInput(std::uint64_t generation_value,
                std::unique_ptr<capability::IAudioSource> audio_source);
  void CleanupLocked();
  void RecordError(const domain::Error& error_value);
  std::string StateFactLocked() const;
};

void RemoteSessionRoute::Impl::RecordError(const domain::Error& error_value) {
  if (error_value.ok()) {
    return;
  }
  const std::lock_guard<std::mutex> lock(error_mutex);
  if (last_error.ok()) {
    last_error = error_value;
  }
}

std::string RemoteSessionRoute::Impl::StateFactLocked() const {
  const bool finished = input_finished.load();
  const bool terminal = terminal_seen.load();
  const std::uint64_t current_generation = generation;
  std::string error_text;
  {
    const std::lock_guard<std::mutex> error_lock(error_mutex);
    if (!last_error.ok()) {
      error_text = " error=" + last_error.message;
    }
  }
  return FactLine(StateName(static_cast<std::uint8_t>(state)), work_id, session_id,
                  current_generation, finished, terminal) +
         error_text;
}

void RemoteSessionRoute::Impl::CleanupLocked() {
  stop_requested.store(true);
  capability::IAudioSource* audio = source.exchange(nullptr);
  if (audio != nullptr) {
    (void)audio->cancel();
  }
  if (input_thread.joinable()) {
    input_thread.join();
  }
  (void)proxy.stop();
  state = State::kIdle;
  input_finished.store(false);
  terminal_seen.store(false);
  cancellation_pending = false;
  cancel_accepted_at = std::chrono::steady_clock::time_point{};
  cancel_deadline = std::chrono::steady_clock::time_point{};
  request_id.clear();
  work_id.clear();
  session_id.clear();
}

domain::Result<ControlResponse> RemoteSessionRoute::Impl::HandleStart(
    const ControlRequest& request) {
  std::lock_guard<std::mutex> lock(mutex);
  if (state == State::kActive || state == State::kStopping) {
    return domain::Result<ControlResponse>::failure(ErrorCode::kBusy, "已有远端会话在途");
  }
  if (state == State::kUnavailable) {
    return domain::Result<ControlResponse>::failure(ErrorCode::kBackendFailure,
                                                    "远端会话路由已经不可用");
  }
  if (!config.input_factory) {
    return domain::Result<ControlResponse>::failure(ErrorCode::kInvalidInput,
                                                    "没有配置输入源工厂");
  }

  CleanupLocked();
  {
    const std::lock_guard<std::mutex> error_lock(error_mutex);
    last_error = domain::Error{};
  }
  stop_requested.store(false);
  input_finished.store(false);
  terminal_seen.store(false);
  cancellation_pending = false;
  cancel_accepted_at = std::chrono::steady_clock::time_point{};
  cancel_deadline = std::chrono::steady_clock::time_point{};
  if (generation == std::numeric_limits<std::uint64_t>::max()) {
    return domain::Result<ControlResponse>::failure(ErrorCode::kBackendFailure,
                                                    "远端会话代际已经耗尽");
  }
  ++generation;
  request_id = request.request_id;
  work_id = request.work_id;
  session_id = request.session_id;

  std::unique_ptr<capability::IAudioSource> audio;
  try {
    audio = config.input_factory();
  } catch (...) {
    return domain::Result<ControlResponse>::failure(ErrorCode::kBackendFailure,
                                                    "创建输入源时发生异常");
  }
  if (audio == nullptr) {
    return domain::Result<ControlResponse>::failure(ErrorCode::kBackendFailure,
                                                    "输入源工厂返回空值");
  }

  const auto started = proxy.start();
  if (!started.ok()) {
    state = State::kIdle;
    return domain::Result<ControlResponse>::failure(started.error.code,
                                                    started.error.message);
  }

  source.store(audio.get());
  state = State::kActive;
  try {
    input_thread = std::thread([this, current = generation,
                                owned = std::move(audio)]() mutable {
      RunInput(current, std::move(owned));
    });
  } catch (...) {
    source.store(nullptr);
    (void)proxy.stop();
    state = State::kUnavailable;
    return domain::Result<ControlResponse>::failure(ErrorCode::kBackendFailure,
                                                    "创建输入线程失败");
  }
  ++stats.starts_accepted;

  const auto response = MakeResponse(
      request, OperationResult::success());
  ControlResponse value = response;
  value.result.error.message = StateFactLocked();
  return domain::Result<ControlResponse>::success(std::move(value));
}

domain::Result<ControlResponse> RemoteSessionRoute::Impl::HandleQuery(
    const ControlRequest& request) {
  std::lock_guard<std::mutex> lock(mutex);
  ControlResponse response = MakeResponse(request, OperationResult::success());
  response.result.error.message = StateFactLocked();
  return domain::Result<ControlResponse>::success(std::move(response));
}

domain::Result<ControlResponse> RemoteSessionRoute::Impl::HandleCancel(
    const ControlRequest& request) {
  std::lock_guard<std::mutex> lock(mutex);
  if (state == State::kIdle || state == State::kCompleted) {
    ++stats.duplicate_cancel_requests;
    ControlResponse response = MakeResponse(request, OperationResult::success());
    response.result.error.message = StateFactLocked();
    return domain::Result<ControlResponse>::success(std::move(response));
  }
  if (state != State::kActive && state != State::kStopping) {
    return domain::Result<ControlResponse>::failure(ErrorCode::kInvalidInput,
                                                    "当前状态不能取消");
  }

  const auto now = std::chrono::steady_clock::now();
  if (state == State::kActive) {
    auto budget = config.cancel_timeout;
    if (request.deadline.count() > 0) {
      budget = std::min(budget, request.deadline);
    }
    cancel_accepted_at = now;
    cancel_deadline = now + budget;
    cancellation_pending = true;
    ++stats.cancel_requests;
  } else {
    // 重复取消不延长原预算：取消必须按第一次受理时承诺的收敛时间报告超时或终态。
    ++stats.duplicate_cancel_requests;
  }

  stop_requested.store(true);
  capability::IAudioSource* audio = source.load();
  if (audio != nullptr) {
    (void)audio->cancel();
  }
  // 输入队列已满时 queue_cancel_stream() 仍返回成功：代理把取消置为待发送事件，
  // 等已排队的输入发完后再发送，因此满队列不会吞掉停止指令。
  (void)proxy.queue_cancel_stream();
  state = State::kStopping;
  ControlResponse response = MakeResponse(request, OperationResult::success());
  response.result.error.message = StateFactLocked();
  return domain::Result<ControlResponse>::success(std::move(response));
}

domain::Result<ControlResponse> RemoteSessionRoute::Impl::HandleExit(
    const ControlRequest& request) {
  std::lock_guard<std::mutex> lock(mutex);
  CleanupLocked();
  ControlResponse response = MakeResponse(request, OperationResult::success());
  response.result.error.message = StateFactLocked();
  return domain::Result<ControlResponse>::success(std::move(response));
}

void RemoteSessionRoute::Impl::RunInput(
    std::uint64_t generation_value,
    std::unique_ptr<capability::IAudioSource> audio_source) {
  const auto opened = audio_source->open();
  if (!opened.ok()) {
    RecordError(opened.error);
    (void)proxy.queue_cancel_stream();
    (void)audio_source->close();
    input_finished.store(true);
    source.store(nullptr);
    return;
  }

  const auto started = proxy.queue_start_stream(config.stream_id, generation_value);
  if (!started.ok()) {
    RecordError(started.error);
    (void)audio_source->close();
    input_finished.store(true);
    source.store(nullptr);
    return;
  }
  // 取消可能早于输入线程执行到 queue_start_stream()：此时第一次 queue_cancel_stream()
  // 会因为没有活动输入流而失败。必须在流建立后补一次取消，否则远端会永远等待输入。
  if (stop_requested.load()) {
    (void)proxy.queue_cancel_stream();
    (void)audio_source->close();
    input_finished.store(true);
    source.store(nullptr);
    return;
  }

  while (!stop_requested.load()) {
    const auto frame = audio_source->read();
    if (frame.ok()) {
      const auto queued = proxy.queue_frame(*frame.value);
      if (!queued.ok()) {
        RecordError(queued.error);
        (void)proxy.queue_cancel_stream();
        break;
      }
      if (config.frame_interval.count() > 0) {
        std::this_thread::sleep_for(config.frame_interval);
      }
      continue;
    }
    if (frame.error.code == ErrorCode::kAlreadyCompleted) {
      (void)proxy.queue_end_stream(0);
      break;
    }
    if (frame.error.code == ErrorCode::kCancelled) {
      (void)proxy.queue_cancel_stream();
      break;
    }
    RecordError(frame.error);
    (void)proxy.queue_cancel_stream();
    break;
  }

  (void)audio_source->close();
  source.store(nullptr);
  input_finished.store(true);
}

RemoteSessionRoute::RemoteSessionRoute(RemoteSessionRouteConfig config) {
  if (!validate_remote_session_route_config(config).ok()) {
    config = RemoteSessionRouteConfig{};
  }
  impl_ = std::make_unique<Impl>(std::move(config));
}

RemoteSessionRoute::~RemoteSessionRoute() {
  if (impl_ != nullptr) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->CleanupLocked();
  }
}

domain::Result<ControlResponse> RemoteSessionRoute::call(
    const ControlRequest& request) {
  if (impl_ == nullptr) {
    return domain::Result<ControlResponse>::failure(ErrorCode::kInvalidInput,
                                                    "远端会话路由尚未构造");
  }
  if (request.operation == "start") {
    return impl_->HandleStart(request);
  }
  if (request.operation == "query") {
    return impl_->HandleQuery(request);
  }
  if (request.operation == "cancel") {
    return impl_->HandleCancel(request);
  }
  if (request.operation == "exit") {
    return impl_->HandleExit(request);
  }
  return domain::Result<ControlResponse>::failure(ErrorCode::kInvalidInput,
                                                  "远端会话路由不支持该操作");
}

std::size_t RemoteSessionRoute::poll_events(gateway::ControlRouteOwner owner,
                                            std::vector<DataEvent>& out,
                                            std::size_t max_events) {
  (void)owner;
  if (impl_ == nullptr || max_events == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl& self = *impl_;
  if (self.state != Impl::State::kActive && self.state != Impl::State::kCompleted &&
      self.state != Impl::State::kStopping) {
    return 0;
  }
  if (self.terminal_seen.load()) {
    // 终态已经交付后，旧会话后续到达的任何输出都不再属于当前请求。主动清空软件队列，
    // 让“输出队列清空”成为可观察事实；socket 中仍在途的数据会由终态过滤丢弃。
    (void)self.proxy.discard_output_events();
    return 0;
  }

  const auto now = std::chrono::steady_clock::now();
  if (self.state == Impl::State::kStopping && self.cancellation_pending &&
      now >= self.cancel_deadline) {
    // 取消预算到期且没有收到终态：交付唯一的结构化超时终态，并请求后台停止。
    // 先封锁后续输出 poll，避免迟到的 done/error 与这条终态形成两个操作结论。
    DataEvent timeout;
    timeout.request_id = self.request_id;
    timeout.session_id = self.session_id;
    timeout.generation = self.generation;
    timeout.type = protocol::DataEventType::kError;
    timeout.end = true;
    timeout.error_code = ErrorCode::kTimeout;
    timeout.message = "取消后远端未在预算内收敛";
    out.push_back(std::move(timeout));

    self.terminal_seen.store(true);
    self.state = Impl::State::kCompleted;
    self.cancellation_pending = false;
    ++self.stats.cancel_timeouts;
    ++self.stats.terminal_events_delivered;
    const auto cancel_started = self.cancel_accepted_at;
    if (cancel_started != std::chrono::steady_clock::time_point{}) {
      self.stats.last_cancel_accept_to_terminal = std::chrono::duration_cast<
          std::chrono::microseconds>(now - cancel_started);
    }
    self.proxy.request_stop();
    (void)self.proxy.discard_output_events();
    if (cancel_started != std::chrono::steady_clock::time_point{}) {
      self.stats.last_cancel_accept_to_queue_clear = std::chrono::duration_cast<
          std::chrono::microseconds>(std::chrono::steady_clock::now() - cancel_started);
    }
    self.cancel_accepted_at = std::chrono::steady_clock::time_point{};
    return 1;
  }

  std::vector<DataEvent> events;
  (void)self.proxy.receive_events(events, max_events, std::chrono::milliseconds{0});
  std::size_t delivered = 0;
  for (DataEvent& event : events) {
    if (self.terminal_seen.load()) {
      // 同一批或后续到达的旧 partial/final/token/PCM/done/error 一律不进入上层；
      // 它们不改变本轮的终态，也不允许把请求复制成第二个操作结论。
      ++self.stats.stale_events_filtered;
      continue;
    }

    const bool terminal = event.end;
    if (terminal) {
      self.terminal_seen.store(true);
      self.state = Impl::State::kCompleted;
      self.cancellation_pending = false;
      ++self.stats.terminal_events_delivered;
    }
    out.push_back(std::move(event));
    ++delivered;

    if (terminal) {
      const auto cancel_started = self.cancel_accepted_at;
      const auto terminal_now = std::chrono::steady_clock::now();
      if (cancel_started != std::chrono::steady_clock::time_point{}) {
        self.stats.last_cancel_accept_to_terminal = std::chrono::duration_cast<
            std::chrono::microseconds>(terminal_now - cancel_started);
      }
      (void)self.proxy.discard_output_events();
      if (cancel_started != std::chrono::steady_clock::time_point{}) {
        self.stats.last_cancel_accept_to_queue_clear = std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() - cancel_started);
      }
      self.cancel_accepted_at = std::chrono::steady_clock::time_point{};
    }
  }
  return delivered;
}

RemoteSessionRouteStats RemoteSessionRoute::stats() const {
  RemoteSessionRouteStats snapshot;
  if (impl_ == nullptr) {
    return snapshot;
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  snapshot = impl_->stats;
  snapshot.cancellation_pending = impl_->cancellation_pending;
  snapshot.terminal_delivered = impl_->terminal_seen.load();
  return snapshot;
}

}  // namespace nexweave::transport

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
  std::shared_ptr<capability::IAudioSource> source;
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
  // 最近一次节点故障的检测时刻。故障终态交付后保留，下一次 start 用它记录清理/就绪/
  // 恢复终态耗时；恢复会话产生终态后清空，避免下一次无关启动继续沿用旧基线。
  std::chrono::steady_clock::time_point fault_detected_at{};
  bool recovery_in_flight = false;
  RemoteSessionRouteStats stats{};

  domain::Result<ControlResponse> HandleStart(const ControlRequest& request);
  domain::Result<ControlResponse> HandleQuery(const ControlRequest& request);
  domain::Result<ControlResponse> HandleCancel(const ControlRequest& request);
  domain::Result<ControlResponse> HandleExit(const ControlRequest& request);
  void RunInput(std::uint64_t generation_value,
                std::shared_ptr<capability::IAudioSource> audio_source);
  void CleanupLocked();
  void RecordError(const domain::Error& error_value);
  std::string StateFactLocked() const;
  std::size_t DeliverFaultLocked(std::vector<DataEvent>& out,
                                 const domain::Error& error);
  void RecordStartFailureLocked(
      std::chrono::steady_clock::time_point recovery_started);
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

std::size_t RemoteSessionRoute::Impl::DeliverFaultLocked(
    std::vector<DataEvent>& out, const domain::Error& error) {
  const auto now = std::chrono::steady_clock::now();
  DataEvent fault;
  fault.request_id = request_id;
  fault.session_id = session_id;
  fault.generation = generation;
  fault.type = protocol::DataEventType::kError;
  fault.end = true;
  fault.error_code =
      error.code == ErrorCode::kNone ? ErrorCode::kBackendFailure : error.code;
  fault.message = error.message.empty() ? std::string("远端 Session 不可用")
                                        : error.message;
  out.push_back(std::move(fault));

  terminal_seen.store(true);
  state = State::kCompleted;
  cancellation_pending = false;
  cancel_accepted_at = std::chrono::steady_clock::time_point{};
  cancel_deadline = std::chrono::steady_clock::time_point{};
  ++stats.faults_detected;
  ++stats.terminal_events_delivered;
  // 新一轮故障会开始新的“检测→清理→就绪→恢复终态”观测，先清掉上一轮耗时，避免
  // 调用方把旧值误读成本轮故障已经完成恢复。
  stats.last_fault_detect_to_cleanup = std::chrono::nanoseconds{0};
  stats.last_fault_detect_to_ready = std::chrono::nanoseconds{0};
  stats.last_fault_detect_to_recovered_terminal = std::chrono::nanoseconds{0};
  stats.last_rebuild_failure_duration = std::chrono::nanoseconds{0};
  if (fault_detected_at == std::chrono::steady_clock::time_point{}) {
    fault_detected_at = now;
  }
  recovery_in_flight = false;
  // 故障可能发生在输入线程阻塞时。先唤醒生产者并撤销它持有的源，下一次 start 才能
  // 在清理阶段安全 join，而不是把一个已经失效的会话留在线程里。
  stop_requested.store(true);
  std::shared_ptr<capability::IAudioSource> audio = source;
  source.reset();
  if (audio != nullptr) {
    (void)audio->cancel();
  }
  RecordError(error);
  return 1;
}

void RemoteSessionRoute::Impl::RecordStartFailureLocked(
    std::chrono::steady_clock::time_point recovery_started) {
  state = State::kUnavailable;
  if (recovery_started != std::chrono::steady_clock::time_point{}) {
    ++stats.rebuilds_failed;
    stats.last_rebuild_failure_duration = std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::steady_clock::now() - recovery_started);
  }
}

void RemoteSessionRoute::Impl::CleanupLocked() {
  stop_requested.store(true);
  std::shared_ptr<capability::IAudioSource> audio = source;
  source.reset();
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
  recovery_in_flight = false;
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

  const auto recovery_started = fault_detected_at;
  CleanupLocked();
  if (recovery_started != std::chrono::steady_clock::time_point{}) {
    stats.last_fault_detect_to_cleanup = std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::steady_clock::now() - recovery_started);
  }
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
    RecordStartFailureLocked(recovery_started);
    return domain::Result<ControlResponse>::failure(ErrorCode::kBackendFailure,
                                                    "远端会话代际已经耗尽");
  }
  ++generation;
  request_id = request.request_id;
  work_id = request.work_id;
  session_id = request.session_id;

  std::unique_ptr<capability::IAudioSource> owned;
  try {
    owned = config.input_factory();
  } catch (...) {
    RecordStartFailureLocked(recovery_started);
    return domain::Result<ControlResponse>::failure(ErrorCode::kBackendFailure,
                                                    "创建输入源时发生异常");
  }
  if (owned == nullptr) {
    RecordStartFailureLocked(recovery_started);
    return domain::Result<ControlResponse>::failure(ErrorCode::kBackendFailure,
                                                    "输入源工厂返回空值");
  }
  // shared_ptr 同时被路由和输入线程持有：取消/清理路径可以在不依赖输入线程是否已经
  // 释放原对象的前提下安全调用 cancel()，不会再出现裸指针 use-after-free。
  std::shared_ptr<capability::IAudioSource> audio = std::move(owned);

  const auto started = proxy.start();
  if (!started.ok()) {
    RecordStartFailureLocked(recovery_started);
    return domain::Result<ControlResponse>::failure(started.error.code,
                                                    started.error.message);
  }

  source = audio;
  state = State::kActive;
  try {
    input_thread = std::thread([this, current = generation,
                                input = std::move(audio)]() mutable {
      RunInput(current, std::move(input));
    });
  } catch (...) {
    source.reset();
    (void)proxy.stop();
    RecordStartFailureLocked(recovery_started);
    return domain::Result<ControlResponse>::failure(ErrorCode::kBackendFailure,
                                                    "创建输入线程失败");
  }
  ++stats.starts_accepted;
  if (recovery_started != std::chrono::steady_clock::time_point{}) {
    stats.last_fault_detect_to_ready = std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::steady_clock::now() - recovery_started);
    ++stats.rebuilds_succeeded;
    recovery_in_flight = true;
  }

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
    // 每次受理新取消都从零开始记录四阶段，避免上一轮耗时被误读成本轮事实。
    stats.last_cancel_accept_to_terminal = std::chrono::nanoseconds{0};
    stats.last_cancel_accept_to_backend_stop = std::chrono::nanoseconds{0};
    stats.last_cancel_accept_to_queue_clear = std::chrono::nanoseconds{0};
    stats.last_cancel_accept_to_total = std::chrono::nanoseconds{0};
    ++stats.cancel_requests;
  } else {
    // 重复取消不延长原预算：取消必须按第一次受理时承诺的收敛时间报告超时或终态。
    ++stats.duplicate_cancel_requests;
  }

  stop_requested.store(true);
  std::shared_ptr<capability::IAudioSource> audio = source;
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
    std::shared_ptr<capability::IAudioSource> audio_source) {
  const auto opened = audio_source->open();
  if (!opened.ok()) {
    RecordError(opened.error);
    (void)proxy.queue_cancel_stream();
    (void)audio_source->close();
    input_finished.store(true);
    return;
  }

  const auto started = proxy.queue_start_stream(config.stream_id, generation_value);
  if (!started.ok()) {
    RecordError(started.error);
    (void)audio_source->close();
    input_finished.store(true);
    return;
  }
  // 取消可能早于输入线程执行到 queue_start_stream()：此时第一次 queue_cancel_stream()
  // 会因为没有活动输入流而失败。必须在流建立后补一次取消，否则远端会永远等待输入。
  if (stop_requested.load()) {
    (void)proxy.queue_cancel_stream();
    (void)audio_source->close();
    input_finished.store(true);
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
    // 终态已经交付后，旧会话后续到达的任何输出都不再属于当前请求。这里继续非阻塞排空
    // 代理队列和 socket，而不是只清本地队列：这样在途旧事件也会被取回、拒绝并记账，
    // 不会因为“路由提前停止读取”而潜伏到下一次 start。没有新事件时 receive_events
    // 立即返回 0，不阻塞调用方。
    std::vector<DataEvent> late;
    const std::size_t late_count =
        self.proxy.receive_events(late, max_events, std::chrono::milliseconds{0});
    self.stats.stale_events_filtered += late_count;
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
          std::chrono::nanoseconds>(now - cancel_started);
    }
    self.proxy.request_stop();
    (void)self.proxy.discard_output_events();
    if (cancel_started != std::chrono::steady_clock::time_point{}) {
      const auto queue_clear_now = std::chrono::steady_clock::now();
      self.stats.last_cancel_accept_to_queue_clear = std::chrono::duration_cast<
          std::chrono::nanoseconds>(queue_clear_now - cancel_started);
      // 超时只证明本地已经给出结构化失败并封锁旧输出，不能把后端停止写成已完成事实。
      self.stats.last_cancel_accept_to_backend_stop = std::chrono::nanoseconds{0};
    }
    self.cancel_accepted_at = std::chrono::steady_clock::time_point{};
    if (cancel_started != std::chrono::steady_clock::time_point{}) {
      self.stats.last_cancel_accept_to_total = std::chrono::duration_cast<
          std::chrono::nanoseconds>(std::chrono::steady_clock::now() - cancel_started);
    }
    return 1;
  }

  std::vector<DataEvent> events;
  (void)self.proxy.receive_events(events, max_events, std::chrono::milliseconds{0});
  if (events.empty() && !self.terminal_seen.load()) {
    // 先让已经到达的 partial/final/token/PCM 通过，再在下一轮 poll 把代理记录到的
    // 节点/传输故障收敛成唯一错误终态。这样旧任务已有输出可以保留，但不会被误当成
    // 新会话的成功收尾。
    const domain::Error proxy_error = self.proxy.last_error();
    if (!proxy_error.ok()) {
      return self.DeliverFaultLocked(out, proxy_error);
    }
  }
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
      if (self.recovery_in_flight &&
          self.fault_detected_at != std::chrono::steady_clock::time_point{}) {
        self.stats.last_fault_detect_to_recovered_terminal =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                terminal_now - self.fault_detected_at);
        self.recovery_in_flight = false;
        self.fault_detected_at = std::chrono::steady_clock::time_point{};
      }
      if (cancel_started != std::chrono::steady_clock::time_point{}) {
        self.stats.last_cancel_accept_to_terminal = std::chrono::duration_cast<
            std::chrono::nanoseconds>(terminal_now - cancel_started);
        // 远端服务端只在会话收尾后发送唯一终态，因此这条事件是“后端停止”的协议上界；
        // 不能据此声称 SDK 内部计算或设备实际静默已经在同一时刻停止。
        self.stats.last_cancel_accept_to_backend_stop =
            self.stats.last_cancel_accept_to_terminal;
      }
      (void)self.proxy.discard_output_events();
      if (cancel_started != std::chrono::steady_clock::time_point{}) {
        const auto queue_clear_now = std::chrono::steady_clock::now();
        self.stats.last_cancel_accept_to_queue_clear = std::chrono::duration_cast<
            std::chrono::nanoseconds>(queue_clear_now - cancel_started);
      }
      self.cancel_accepted_at = std::chrono::steady_clock::time_point{};
      if (cancel_started != std::chrono::steady_clock::time_point{}) {
        self.stats.last_cancel_accept_to_total = std::chrono::duration_cast<
            std::chrono::nanoseconds>(std::chrono::steady_clock::now() - cancel_started);
      }
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
  const RemoteSessionProxyStats proxy_stats = impl_->proxy.stats();
  snapshot.transport_stale_events_filtered = proxy_stats.stale_output_events_filtered;
  snapshot.discarded_output_events = proxy_stats.discarded_output_events;
  snapshot.cancellation_pending = impl_->cancellation_pending;
  snapshot.terminal_delivered = impl_->terminal_seen.load();
  return snapshot;
}

}  // namespace nexweave::transport

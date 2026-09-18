#include "remote_session_proxy.hpp"

#include <fstream>
#include <utility>

#include "bounded_queue.hpp"
#include "terminal_cache.hpp"

namespace nexweave::transport {
namespace {

using domain::ErrorCode;
using domain::OperationResult;
using protocol::DataEvent;
using runtime::BoundedQueue;
using runtime::BoundedQueueConfig;
using runtime::QueuePopStatus;
using runtime::QueuePushStatus;
using runtime::TerminalCache;
using runtime::TerminalCacheConfig;

std::string ReadEndpointFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::string();
  }
  std::string endpoint;
  std::getline(file, endpoint);
  while (!endpoint.empty() &&
         (endpoint.back() == '\r' || endpoint.back() == '\n' || endpoint.back() == ' ')) {
    endpoint.pop_back();
  }
  return endpoint;
}

BoundedQueueConfig MakeInputQueueConfig(const RemoteSessionProxyConfig& config) {
  BoundedQueueConfig queue;
  queue.capacity = config.max_pending_input_events;
  queue.drop_oldest_on_full = false;
  return queue;
}

BoundedQueueConfig MakeOutputQueueConfig(const RemoteSessionProxyConfig& config) {
  BoundedQueueConfig queue;
  queue.capacity = config.max_pending_output_events;
  queue.drop_oldest_on_full = false;
  return queue;
}

TerminalCacheConfig MakeTerminalCacheConfig(const RemoteSessionProxyConfig& config) {
  TerminalCacheConfig cache;
  cache.capacity = config.terminal_cache_capacity;
  cache.retention = config.terminal_retention;
  return cache;
}

}  // namespace

domain::OperationResult validate_remote_session_proxy_config(
    const RemoteSessionProxyConfig& config) {
  const auto child_valid = runtime::validate_child_process_spec(config.child);
  if (!child_valid.ok()) {
    return child_valid;
  }
  const auto config_valid = runtime::validate_child_process_config(config.child_config);
  if (!config_valid.ok()) {
    return config_valid;
  }
  if (config.endpoint_file.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "端点文件路径不能为空");
  }
  if (config.pump_interval.count() <= 0 || config.ready_timeout.count() <= 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "代理等待预算必须为正");
  }
  if (config.max_pending_input_events == 0 || config.max_pending_output_events == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "代理队列容量必须为正");
  }
  if (config.terminal_cache_capacity == 0 || config.terminal_retention.count() <= 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "终态缓存容量与保留期限必须为正");
  }
  return validate_zmq_data_config(config.data);
}

struct RemoteSessionProxy::Impl {
  explicit Impl(RemoteSessionProxyConfig config_value)
      : config(std::move(config_value)),
        child(config.child_config),
        channel(config.data) {}

  RemoteSessionProxyConfig config;
  runtime::ChildProcess child{config.child_config};
  ZmqDataChannel channel;
  std::atomic<bool> running{false};
  std::atomic<bool> stop_requested{false};
  std::thread io_thread;

  mutable std::mutex mutex;
  std::unique_ptr<BoundedQueue<runtime::InputStreamEvent>> inbound_queue;
  std::unique_ptr<BoundedQueue<DataEvent>> outbound_queue;
  std::unique_ptr<TerminalCache<DataEvent>> terminal_cache;
  domain::Error error{};
  RemoteSessionProxyStats stats{};
  bool stream_started = false;
  bool stream_ended = false;
  // 取消事件绕过有界输入队列：置位后 I/O 线程把此前排队的输入发完，再补发这条取消。
  bool cancel_pending = false;
  std::uint64_t pending_cancel_sequence = 0;
  std::string stream_id;
  std::uint64_t generation = 0;
  std::uint64_t next_sequence = 0;

  domain::OperationResult Start();
  OperationResult Stop() noexcept;
  void RequestStop() noexcept;
  std::size_t DiscardOutputEvents() noexcept;
  domain::OperationResult QueueStart(std::string id, std::uint64_t generation_value);
  domain::OperationResult QueueFrame(const domain::AudioFrame& frame);
  domain::OperationResult QueueEnd(std::size_t valid_samples,
                                  std::optional<domain::AudioFrame> tail);
  domain::OperationResult QueueCancel();
  std::size_t ReceiveEvents(std::vector<DataEvent>& out, std::size_t max_events,
                            std::chrono::milliseconds timeout);
  domain::Result<DataEvent> QueryTerminal(const std::string& request_id,
                                          std::uint64_t generation_value) const;
  void IoLoop();
  bool PushInputLocked(const runtime::InputStreamEvent& event);
  void StoreTerminal(const DataEvent& event);
  void RecordError(const domain::Error& error_value);
};

domain::OperationResult RemoteSessionProxy::Impl::Start() {
  bool expected = false;
  if (!running.compare_exchange_strong(expected, true)) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted, "远端代理已经启动");
  }
  stop_requested.store(false);
  {
    const std::lock_guard<std::mutex> lock(mutex);
    inbound_queue = std::make_unique<BoundedQueue<runtime::InputStreamEvent>>(
        MakeInputQueueConfig(config));
    outbound_queue = std::make_unique<BoundedQueue<DataEvent>>(
        MakeOutputQueueConfig(config));
    terminal_cache = std::make_unique<TerminalCache<DataEvent>>(
        MakeTerminalCacheConfig(config));
    error = domain::Error{};
    stats = RemoteSessionProxyStats{};
    stream_started = false;
    stream_ended = false;
    cancel_pending = false;
    pending_cancel_sequence = 0;
    stream_id.clear();
    generation = 0;
    next_sequence = 0;
  }

  std::remove(config.endpoint_file.c_str());

  runtime::ChildProcessSpec spec = config.child;
  spec.environment.push_back(std::string(kRemoteSessionEndpointFileEnvironment) + "=" +
                             config.endpoint_file);
  const auto started = child.start(spec);
  if (!started.ok()) {
    running.store(false);
    RecordError(started.error);
    return OperationResult{started.error};
  }

  const std::string endpoint = ReadEndpointFile(config.endpoint_file);
  if (endpoint.empty()) {
    const OperationResult failed =
        OperationResult::failure(ErrorCode::kBackendFailure, "子进程没有写出数据面端点");
    RecordError(failed.error);
    child.stop(started.value.value());
    running.store(false);
    return failed;
  }

  const auto connected = channel.connect(endpoint);
  if (!connected.ok()) {
    RecordError(connected.error);
    child.stop(started.value.value());
    running.store(false);
    return connected;
  }
  const auto ready = channel.wait_ready(config.ready_timeout);
  if (!ready.ok()) {
    RecordError(ready.error);
    channel.close();
    child.stop(started.value.value());
    running.store(false);
    return ready;
  }

  io_thread = std::thread([this] { IoLoop(); });
  return OperationResult::success();
}

OperationResult RemoteSessionProxy::Impl::Stop() noexcept {
  try {
    if (!running.load()) {
      return OperationResult::success();
    }
    // 先在途输入有机会送达，再放入取消事件；最后关闭队列唤醒 I/O 线程。
    (void)QueueCancel();
    stop_requested.store(true);
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (inbound_queue != nullptr) {
        inbound_queue->close();
      }
      if (outbound_queue != nullptr) {
        outbound_queue->close();
      }
    }
    if (io_thread.joinable()) {
      io_thread.join();
    }
    channel.close();
    const auto stopped = child.stop();
    running.store(false);
    if (!stopped.ok()) {
      RecordError(stopped.error);
      return OperationResult{stopped.error};
    }
    return OperationResult::success();
  } catch (...) {
    running.store(false);
    return OperationResult::failure(ErrorCode::kBackendFailure,
                                    "停止远端代理时发生异常");
  }
}

void RemoteSessionProxy::Impl::RequestStop() noexcept {
  try {
    stop_requested.store(true);
    {
      const std::lock_guard<std::mutex> lock(mutex);
      ++stats.requested_stops;
      if (inbound_queue != nullptr) {
        // 取消超时后不再等待在途输入送达：先丢弃软件队列，再请求子进程停止。
        // 已经交给 channel 的消息无法撤回，但调用方不会再消费它们的输出。
        (void)inbound_queue->clear();
        inbound_queue->close();
      }
      if (outbound_queue != nullptr) {
        outbound_queue->close();
      }
    }
    child.request_stop();
  } catch (...) {
    // 非阻塞停止路径只做尽力回收；异常不能从析构/超时收敛路径穿透。
  }
}

std::size_t RemoteSessionProxy::Impl::DiscardOutputEvents() noexcept {
  try {
    const std::lock_guard<std::mutex> lock(mutex);
    if (outbound_queue == nullptr) {
      return 0;
    }
    const std::size_t removed = outbound_queue->clear();
    stats.discarded_output_events += removed;
    return removed;
  } catch (...) {
    return 0;
  }
}

bool RemoteSessionProxy::Impl::PushInputLocked(const runtime::InputStreamEvent& event) {
  // 前置条件：调用方已持有 mutex。
  if (stop_requested.load() || inbound_queue == nullptr) {
    return false;
  }
  const QueuePushStatus pushed = inbound_queue->try_push(event);
  if (pushed == QueuePushStatus::kAccepted) {
    return true;
  }
  if (pushed == QueuePushStatus::kRejectedFull) {
    ++stats.input_queue_full;
    return false;
  }
  return false;
}

domain::OperationResult RemoteSessionProxy::Impl::QueueStart(std::string id,
                                                             std::uint64_t generation_value) {
  if (id.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "输入流标识不能为空");
  }
  runtime::InputStreamEvent event;
  event.stream_id = std::move(id);
  event.generation = generation_value;
  event.sequence = 0;
  event.kind = runtime::InputEventKind::kStart;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    if (!running.load() || stop_requested.load()) {
      return OperationResult::failure(ErrorCode::kAlreadyCompleted, "远端代理尚未运行");
    }
    if (stream_started && !stream_ended) {
      return OperationResult::failure(ErrorCode::kAlreadyCompleted, "输入流已经开始");
    }
    if (!PushInputLocked(event)) {
      return OperationResult::failure(ErrorCode::kBackendFailure, "输入事件队列已满");
    }
    stream_started = true;
    stream_ended = false;
    cancel_pending = false;
    pending_cancel_sequence = 0;
    stream_id = event.stream_id;
    generation = generation_value;
    next_sequence = 1;
    ++stats.queued_input_events;
  }
  return OperationResult::success();
}

domain::OperationResult RemoteSessionProxy::Impl::QueueFrame(
    const domain::AudioFrame& frame) {
  const auto valid = domain::validate_audio_frame(frame);
  if (!valid.ok()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "只接受合法固定音频帧");
  }
  runtime::InputStreamEvent event;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    if (!running.load() || stop_requested.load()) {
      return OperationResult::failure(ErrorCode::kAlreadyCompleted, "远端代理尚未运行");
    }
    if (!stream_started || stream_ended) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "输入流尚未开始或已经结束");
    }
    event.stream_id = stream_id;
    event.generation = generation;
    event.sequence = next_sequence;
    event.kind = runtime::InputEventKind::kFrame;
    event.frame = frame;
    event.valid_samples = domain::kAudioFrameSamples;
    if (!PushInputLocked(event)) {
      return OperationResult::failure(ErrorCode::kBackendFailure, "输入事件队列已满");
    }
    ++next_sequence;
    ++stats.queued_input_events;
  }
  return OperationResult::success();
}

domain::OperationResult RemoteSessionProxy::Impl::QueueEnd(
    std::size_t valid_samples, std::optional<domain::AudioFrame> tail) {
  if (valid_samples == 0 && tail.has_value()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "零样本结束不能携带尾帧");
  }
  if (valid_samples > domain::kAudioFrameSamples) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "结束有效样本数越界");
  }
  if (valid_samples > 0) {
    if (!tail.has_value()) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "短尾结束必须携带补零尾帧");
    }
    const auto valid = domain::validate_audio_frame(*tail);
    if (!valid.ok()) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "补零尾帧不符合音频合同");
    }
  }

  runtime::InputStreamEvent event;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    if (!running.load() || stop_requested.load()) {
      return OperationResult::failure(ErrorCode::kAlreadyCompleted, "远端代理尚未运行");
    }
    if (!stream_started || stream_ended) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "输入流尚未开始或已经结束");
    }
    event.stream_id = stream_id;
    event.generation = generation;
    event.sequence = next_sequence;
    event.kind = runtime::InputEventKind::kEnd;
    event.valid_samples = valid_samples;
    if (tail.has_value()) {
      event.frame = std::move(*tail);
    }
    if (!PushInputLocked(event)) {
      return OperationResult::failure(ErrorCode::kBackendFailure, "输入事件队列已满");
    }
    stream_ended = true;
    ++next_sequence;
    ++stats.queued_input_events;
  }
  return OperationResult::success();
}

domain::OperationResult RemoteSessionProxy::Impl::QueueCancel() {
  const std::lock_guard<std::mutex> lock(mutex);
  if (!running.load() || stop_requested.load()) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted, "远端代理尚未运行");
  }
  if (!stream_started) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "输入流尚未开始");
  }
  // 取消是流级终态：已经结束的流重复取消返回成功，让调用方的幂等重试不会在
  // 取消与自然完成竞态时被误报为协议错误。取消事件不进入有界队列，因此队列已满
  // 也不会丢失停止指令；I/O 线程会在排空此前输入后补发它。
  if (cancel_pending || stream_ended) {
    return OperationResult::success();
  }
  cancel_pending = true;
  pending_cancel_sequence = next_sequence;
  stream_ended = true;
  ++next_sequence;
  ++stats.queued_input_events;
  return OperationResult::success();
}

std::size_t RemoteSessionProxy::Impl::ReceiveEvents(
    std::vector<DataEvent>& out, std::size_t max_events,
    std::chrono::milliseconds timeout) {
  if (max_events == 0) {
    return 0;
  }
  BoundedQueue<DataEvent>* queue = nullptr;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    queue = outbound_queue.get();
  }
  if (queue == nullptr) {
    return 0;
  }
  std::size_t count = 0;
  DataEvent event;
  const QueuePopStatus first = queue->pop_for(event, timeout);
  if (first == QueuePopStatus::kItem) {
    out.push_back(std::move(event));
    ++count;
  } else {
    return 0;
  }
  while (count < max_events) {
    DataEvent next;
    const QueuePopStatus popped = queue->pop_for(next, std::chrono::milliseconds(0));
    if (popped != QueuePopStatus::kItem) {
      break;
    }
    out.push_back(std::move(next));
    ++count;
  }
  return count;
}

domain::Result<DataEvent> RemoteSessionProxy::Impl::QueryTerminal(
    const std::string& request_id, std::uint64_t generation_value) const {
  std::lock_guard<std::mutex> lock(mutex);
  if (terminal_cache == nullptr) {
    return domain::Result<DataEvent>::failure(ErrorCode::kAlreadyCompleted,
                                              "终态缓存尚不可用");
  }
  return terminal_cache->Query(request_id, generation_value);
}

void RemoteSessionProxy::Impl::StoreTerminal(const DataEvent& event) {
  if (!event.end) {
    return;
  }
  const std::lock_guard<std::mutex> lock(mutex);
  if (terminal_cache != nullptr) {
    terminal_cache->Store(event.request_id, event.generation, event);
  }
}

void RemoteSessionProxy::Impl::IoLoop() {
  while (true) {
    runtime::InputStreamEvent event;
    const QueuePopStatus popped = inbound_queue->try_pop(event);
    if (popped == QueuePopStatus::kItem) {
      const auto sent = channel.send_input(event);
      if (!sent.ok()) {
        RecordError(sent.error);
        {
          const std::lock_guard<std::mutex> lock(mutex);
          ++stats.send_failures;
        }
        child.request_stop();
        break;
      }
      {
        const std::lock_guard<std::mutex> lock(mutex);
        ++stats.sent_input_events;
      }
      continue;
    }
    if (popped == QueuePopStatus::kClosed) {
      break;
    }

    // 取消事件绕过有界输入队列：只有在此前排队的输入事件都已经弹出并发送后，
    // 才补发这条取消。这样队列满时也不会丢失停止指令，而且线上的 sequence
    // 仍保持连续、取消排在已提交音频之后。
    runtime::InputStreamEvent cancel_event;
    bool has_pending_cancel = false;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (cancel_pending) {
        cancel_event.stream_id = stream_id;
        cancel_event.generation = generation;
        cancel_event.sequence = pending_cancel_sequence;
        cancel_event.kind = runtime::InputEventKind::kCancel;
        cancel_pending = false;
        has_pending_cancel = true;
      }
    }
    if (has_pending_cancel) {
      const auto sent = channel.send_input(cancel_event);
      if (!sent.ok()) {
        RecordError(sent.error);
        {
          const std::lock_guard<std::mutex> lock(mutex);
          ++stats.send_failures;
        }
        child.request_stop();
        break;
      }
      {
        const std::lock_guard<std::mutex> lock(mutex);
        ++stats.sent_input_events;
      }
      continue;
    }

    if (stop_requested.load() && inbound_queue->empty()) {
      break;
    }

    const auto received = channel.receive_output(config.pump_interval);
    if (received.ok()) {
      const DataEvent& event_value = received.value.value();
      StoreTerminal(event_value);
      const QueuePushStatus pushed = outbound_queue->try_push(event_value);
      if (pushed == QueuePushStatus::kAccepted) {
        const std::lock_guard<std::mutex> lock(mutex);
        ++stats.received_output_events;
        continue;
      }
      if (pushed == QueuePushStatus::kRejectedFull) {
        {
          const std::lock_guard<std::mutex> lock(mutex);
          ++stats.output_queue_full;
          if (error.ok()) {
            error = domain::Error{ErrorCode::kBackendFailure, "远端输出队列已满"};
          }
        }
        child.request_stop();
        break;
      }
      break;
    }

    if (received.error.code == ErrorCode::kTimeout) {
      const std::lock_guard<std::mutex> lock(mutex);
      ++stats.receive_timeouts;
      continue;
    }

    // 传输层已经在同一 request/session 上判定这条输出属于旧代际或重复终态。
    // 这类事件必须被丢弃而不是升级成进程故障，否则一个迟到的旧结果会终止
    // 正在服务新轮次的子进程；过滤只记账，不影响 last_error()。
    if (received.error.code == ErrorCode::kCancelled ||
        received.error.code == ErrorCode::kAlreadyCompleted) {
      const std::lock_guard<std::mutex> lock(mutex);
      ++stats.stale_output_events_filtered;
      continue;
    }

    RecordError(received.error);
    child.request_stop();
    break;
  }
  outbound_queue->close();
}

void RemoteSessionProxy::Impl::RecordError(const domain::Error& error_value) {
  const std::lock_guard<std::mutex> lock(mutex);
  if (error.ok()) {
    error = error_value;
  }
}

RemoteSessionProxy::RemoteSessionProxy(RemoteSessionProxyConfig config) {
  if (!validate_remote_session_proxy_config(config).ok()) {
    config = RemoteSessionProxyConfig{};
  }
  impl_ = std::make_unique<Impl>(std::move(config));
}

RemoteSessionProxy::~RemoteSessionProxy() {
  stop();
}

domain::OperationResult RemoteSessionProxy::start() {
  return impl_->Start();
}

domain::OperationResult RemoteSessionProxy::stop() noexcept {
  return impl_->Stop();
}

void RemoteSessionProxy::request_stop() noexcept {
  if (impl_ != nullptr) {
    impl_->RequestStop();
  }
}

std::size_t RemoteSessionProxy::discard_output_events() noexcept {
  if (impl_ == nullptr) {
    return 0;
  }
  return impl_->DiscardOutputEvents();
}

domain::OperationResult RemoteSessionProxy::queue_start_stream(std::string stream_id,
                                                               std::uint64_t generation) {
  if (impl_ == nullptr) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "代理尚未构造");
  }
  return impl_->QueueStart(std::move(stream_id), generation);
}

domain::OperationResult RemoteSessionProxy::queue_frame(const domain::AudioFrame& frame) {
  if (impl_ == nullptr) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "代理尚未构造");
  }
  return impl_->QueueFrame(frame);
}

domain::OperationResult RemoteSessionProxy::queue_end_stream(
    std::size_t valid_samples, std::optional<domain::AudioFrame> tail) {
  if (impl_ == nullptr) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "代理尚未构造");
  }
  return impl_->QueueEnd(valid_samples, std::move(tail));
}

domain::OperationResult RemoteSessionProxy::queue_cancel_stream() {
  if (impl_ == nullptr) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "代理尚未构造");
  }
  return impl_->QueueCancel();
}

std::size_t RemoteSessionProxy::receive_events(std::vector<protocol::DataEvent>& out,
                                               std::size_t max_events,
                                               std::chrono::milliseconds timeout) {
  if (impl_ == nullptr) {
    return 0;
  }
  return impl_->ReceiveEvents(out, max_events, timeout);
}

domain::Result<protocol::DataEvent> RemoteSessionProxy::query_terminal(
    const std::string& request_id, std::uint64_t generation) const {
  if (impl_ == nullptr) {
    return domain::Result<protocol::DataEvent>::failure(ErrorCode::kAlreadyCompleted,
                                                        "代理尚未构造");
  }
  return impl_->QueryTerminal(request_id, generation);
}

domain::Error RemoteSessionProxy::last_error() const {
  if (impl_ == nullptr) {
    return domain::Error{};
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->error;
}

bool RemoteSessionProxy::running() const noexcept {
  return impl_ != nullptr && impl_->running.load();
}

RemoteSessionProxyStats RemoteSessionProxy::stats() const {
  if (impl_ == nullptr) {
    return RemoteSessionProxyStats{};
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  RemoteSessionProxyStats snapshot = impl_->stats;
  if (impl_->inbound_queue != nullptr) {
    const auto queue_stats = impl_->inbound_queue->stats();
    snapshot.input_queue_capacity = queue_stats.capacity;
    snapshot.input_queue_peak = queue_stats.peak_size;
  }
  if (impl_->outbound_queue != nullptr) {
    const auto queue_stats = impl_->outbound_queue->stats();
    snapshot.output_queue_capacity = queue_stats.capacity;
    snapshot.output_queue_peak = queue_stats.peak_size;
    snapshot.output_queue_size = queue_stats.current_size;
  }
  if (impl_->terminal_cache != nullptr) {
    const auto terminal_stats = impl_->terminal_cache->stats();
    snapshot.terminal_cache_capacity = terminal_stats.capacity;
    snapshot.terminal_cache_entries = terminal_stats.live_entries;
    snapshot.terminal_cache_peak_entries = terminal_stats.peak_live_entries;
    snapshot.terminal_cache_expired_markers = terminal_stats.expired_markers;
  }
  return snapshot;
}

}  // namespace nexweave::transport

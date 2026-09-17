#include "remote_session_server.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

#include "bounded_queue.hpp"

namespace nexweave::transport {
namespace {

using domain::ErrorCode;
using domain::OperationResult;
using protocol::DataEvent;
using protocol::DataEventType;
using runtime::BoundedQueue;
using runtime::BoundedQueueConfig;
using runtime::QueuePopStatus;
using runtime::QueuePushStatus;
using runtime::QueuedAudioSource;
using runtime::SessionApp;
using runtime::SessionAppObserver;
using runtime::SessionTurnResult;

std::vector<std::uint8_t> EncodePcmBytes(const domain::AudioFrame& frame) {
  std::vector<std::uint8_t> bytes(domain::kAudioFrameBytes, 0);
  for (std::size_t index = 0; index < frame.samples.size(); ++index) {
    const std::uint16_t sample = static_cast<std::uint16_t>(frame.samples[index]);
    bytes[index * 2] = static_cast<std::uint8_t>(sample & 0xFFU);
    bytes[index * 2 + 1] = static_cast<std::uint8_t>((sample >> 8U) & 0xFFU);
  }
  return bytes;
}

DataEvent MakeEvent(const std::string& request_id, const std::string& session_id,
                    std::uint64_t generation, std::uint64_t sequence,
                    DataEventType type) {
  DataEvent event;
  event.request_id = request_id;
  event.session_id = session_id;
  event.generation = generation;
  event.sequence = sequence;
  event.type = type;
  return event;
}

}  // namespace

domain::OperationResult validate_remote_session_server_config(
    const RemoteSessionServerConfig& config) {
  if (config.endpoint.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "数据面端点不能为空");
  }
  if (config.ready_timeout.count() <= 0 ||
      config.receive_poll_interval.count() <= 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "服务端等待预算必须为正");
  }
  if (config.output_queue_capacity == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "输出事件容量必须为正");
  }
  return validate_zmq_data_config(config.data);
}

namespace {

RemoteSessionServerConfig EffectiveServerConfig(RemoteSessionServerConfig config) {
  if (!validate_remote_session_server_config(config).ok()) {
    return RemoteSessionServerConfig{};
  }
  return config;
}

BoundedQueueConfig MakeOutputQueueConfig(const RemoteSessionServerConfig& config) {
  BoundedQueueConfig queue;
  queue.capacity = config.output_queue_capacity;
  queue.drop_oldest_on_full = false;
  return queue;
}

}  // namespace

struct RemoteSessionServer::Impl {
  Impl(SessionApp& app_value, QueuedAudioSource& input_value,
       RemoteSessionServerConfig config_value)
      : app(app_value),
        input(input_value),
        config(std::move(config_value)),
        channel(config.data),
        outbound(MakeOutputQueueConfig(config)) {}

  class Observer final : public SessionAppObserver {
   public:
    explicit Observer(Impl& owner) : owner_(owner) {}

    void on_turn(const SessionTurnResult& result) override {
      owner_.EnqueueTurn(result);
    }

   private:
    Impl& owner_;
  };

  SessionApp& app;
  QueuedAudioSource& input;
  RemoteSessionServerConfig config;
  ZmqDataChannel channel;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> bind_called{false};
  std::atomic<bool> serve_called{false};
  bool bound = false;
  std::string endpoint;
  std::thread worker;
  mutable std::mutex mutex;
  BoundedQueue<DataEvent> outbound;
  bool app_finished = false;
  bool terminal_sent = false;
  bool output_overflow = false;
  bool cancel_requested = false;
  domain::Error error{};
  std::string stream_id;
  RemoteSessionServerStats stats{};

  domain::OperationResult Bind();
  domain::OperationResult Serve();
  void RunApp();
  void RequestStop();
  void EnqueueTurn(const SessionTurnResult& result);
  bool EnqueueEvent(const DataEvent& event);
  bool DrainOutbound();
  void EnqueueSessionErrorLocked(const domain::Error& error_value);
};

domain::OperationResult RemoteSessionServer::Impl::Bind() {
  if (bind_called.exchange(true)) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted, "服务端已经绑定");
  }
  const auto bound_result = channel.bind(config.endpoint);
  if (!bound_result.ok()) {
    bind_called.store(false);
    return bound_result;
  }
  bound = true;
  endpoint = channel.bound_endpoint();
  return OperationResult::success();
}

domain::OperationResult RemoteSessionServer::Impl::Serve() {
  if (!bound) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "服务端尚未绑定");
  }
  if (serve_called.exchange(true)) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted, "服务端已经开始运行");
  }

  const auto ready = channel.wait_ready(config.ready_timeout);
  if (!ready.ok()) {
    return ready;
  }

  worker = std::thread([this] { RunApp(); });

  while (true) {
    if (!DrainOutbound()) {
      RequestStop();
    }

    bool finished = false;
    bool out_empty = false;
    bool must_stop = false;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      finished = app_finished;
      out_empty = outbound.empty();
      must_stop = stop_requested.load() || !error.ok();
    }

    if (must_stop) {
      if (stop_requested.load()) {
        const std::lock_guard<std::mutex> lock(mutex);
        cancel_requested = true;
      }
      // 停止信号一旦出现，必须先唤醒可能阻塞在空队列上的会话线程并封锁当前输出，
      // 再决定是否退出循环；否则 join 会等待一个永远不会自己醒来的 read。
      input.cancel();
      app.cancel_turn();
      app.request_stop();
    }

    if (finished && out_empty) {
      break;
    }
    if (finished && !out_empty) {
      // 应用已经结束，只是还有事件没有发出去；下一步循环继续 DrainOutbound。
      continue;
    }
    if (!error.ok()) {
      // 传输错误已经决定收敛，不再继续读入；让会话线程在被取消后完成收尾。
      std::this_thread::sleep_for(config.receive_poll_interval);
      continue;
    }

    const auto received = channel.receive_input(config.receive_poll_interval);
    if (received.ok()) {
      const runtime::InputStreamEvent& event = received.value.value();
      {
        const std::lock_guard<std::mutex> lock(mutex);
        ++stats.received_input_events;
      }
      switch (event.kind) {
        case runtime::InputEventKind::kStart: {
          const std::lock_guard<std::mutex> lock(mutex);
          stream_id = event.stream_id;
          break;
        }
        case runtime::InputEventKind::kFrame: {
          if (!event.frame.has_value()) {
            {
              const std::lock_guard<std::mutex> lock(mutex);
              ++stats.input_rejected;
            }
            RequestStop();
            break;
          }
          const auto pushed = input.push(*event.frame);
          if (!pushed.ok()) {
            {
              const std::lock_guard<std::mutex> lock(mutex);
              ++stats.input_rejected;
            }
            {
              const std::lock_guard<std::mutex> lock(mutex);
              if (error.ok()) {
                error = pushed.error;
              }
            }
            RequestStop();
            break;
          }
          {
            const std::lock_guard<std::mutex> lock(mutex);
            ++stats.received_input_frames;
          }
          break;
        }
        case runtime::InputEventKind::kEnd: {
          if (event.frame.has_value() && event.valid_samples > 0) {
            const auto pushed = input.push(*event.frame);
            if (!pushed.ok()) {
              {
                const std::lock_guard<std::mutex> lock(mutex);
                ++stats.input_rejected;
              }
              {
                const std::lock_guard<std::mutex> lock(mutex);
                if (error.ok()) {
                  error = pushed.error;
                }
              }
              RequestStop();
              break;
            }
            {
              const std::lock_guard<std::mutex> lock(mutex);
              ++stats.received_input_frames;
            }
          }
          input.end_input();
          break;
        }
        case runtime::InputEventKind::kCancel: {
          {
            const std::lock_guard<std::mutex> lock(mutex);
            cancel_requested = true;
          }
          input.cancel();
          app.cancel_turn();
          app.request_stop();
          break;
        }
      }
      continue;
    }

    if (received.error.code == ErrorCode::kTimeout) {
      {
        const std::lock_guard<std::mutex> lock(mutex);
        ++stats.receive_timeouts;
      }
      continue;
    }

    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (error.ok()) {
        error = received.error;
      }
    }
    RequestStop();
  }

  if (worker.joinable()) {
    worker.join();
  }
  DrainOutbound();

  {
    const std::lock_guard<std::mutex> lock(mutex);
    if (!terminal_sent) {
      if (cancel_requested && error.ok()) {
        error = domain::Error{ErrorCode::kCancelled, "远端会话被取消"};
      } else if (error.ok()) {
        error = domain::Error{ErrorCode::kBackendFailure, "远端会话没有产生终态事件"};
      }
      if (!output_overflow) {
        EnqueueSessionErrorLocked(error);
      }
    }
  }
  DrainOutbound();

  domain::Error final_error;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    final_error = error;
  }
  if (!final_error.ok()) {
    return OperationResult{final_error};
  }
  return OperationResult::success();
}

void RemoteSessionServer::Impl::RunApp() {
  Observer observer(*this);
  const runtime::SessionAppRunResult result = app.run(&observer);
  {
    const std::lock_guard<std::mutex> lock(mutex);
    app_finished = true;
    if (!result.error.ok() && error.ok()) {
      error = result.error;
    }
    if (!result.cleanup_error.ok() && error.ok()) {
      error = result.cleanup_error;
    }
  }
}

void RemoteSessionServer::Impl::RequestStop() {
  stop_requested.store(true);
}

bool RemoteSessionServer::Impl::EnqueueEvent(const DataEvent& event) {
  const std::lock_guard<std::mutex> lock(mutex);
  const QueuePushStatus pushed = outbound.try_push(event);
  if (pushed == QueuePushStatus::kAccepted) {
    return true;
  }
  if (pushed == QueuePushStatus::kRejectedFull) {
    output_overflow = true;
    if (error.ok()) {
      error = domain::Error{ErrorCode::kBackendFailure, "远端输出队列已满"};
    }
    input.cancel();
    app.request_stop();
  }
  return false;
}

void RemoteSessionServer::Impl::EnqueueTurn(const SessionTurnResult& result) {
  std::string session_id;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    session_id = stream_id.empty() ? std::string("remote-session") : stream_id;
  }
  const std::string request_id =
      result.request_id.empty() ? session_id + "-session-turn" : result.request_id;
  std::uint64_t sequence = 0;

  if (!result.text.empty()) {
    DataEvent event = MakeEvent(request_id, session_id, result.generation, sequence++,
                                DataEventType::kFinal);
    event.text = result.text;
    if (!EnqueueEvent(event)) {
      return;
    }
  }

  std::size_t frame_index = 0;
  for (const domain::AudioFrame& frame : result.pcm_frames) {
    DataEvent event = MakeEvent(request_id, session_id, result.generation, sequence++,
                                DataEventType::kPcm);
    event.frame_index = static_cast<std::uint32_t>(frame_index++);
    event.pcm = EncodePcmBytes(frame);
    event.expected_pcm_bytes = domain::kAudioFrameBytes;
    if (!EnqueueEvent(event)) {
      return;
    }
  }

  DataEvent terminal = MakeEvent(request_id, session_id, result.generation, sequence,
                                 result.completed ? DataEventType::kDone
                                                  : DataEventType::kError);
  terminal.end = true;
  if (!result.completed) {
    terminal.error_code = result.cancelled
                              ? ErrorCode::kCancelled
                              : (result.error.ok() ? ErrorCode::kBackendFailure
                                                   : result.error.code);
    terminal.message = result.error.ok() ? std::string("远端会话未完成")
                                         : result.error.message;
  }
  if (!EnqueueEvent(terminal)) {
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(mutex);
    terminal_sent = true;
  }
}

bool RemoteSessionServer::Impl::DrainOutbound() {
  while (true) {
    DataEvent event;
    const QueuePopStatus popped = outbound.try_pop(event);
    if (popped == QueuePopStatus::kEmpty || popped == QueuePopStatus::kClosed) {
      return true;
    }
    const auto sent = channel.send_output(event);
    if (!sent.ok()) {
      {
        const std::lock_guard<std::mutex> lock(mutex);
        ++stats.send_failures;
        if (error.ok()) {
          error = sent.error;
        }
      }
      return false;
    }
    const std::lock_guard<std::mutex> lock(mutex);
    ++stats.sent_output_events;
  }
}

void RemoteSessionServer::Impl::EnqueueSessionErrorLocked(const domain::Error& error_value) {
  const std::string session_id =
      stream_id.empty() ? std::string("remote-session") : stream_id;
  DataEvent event = MakeEvent(session_id + "-session", session_id, 0, 0,
                              DataEventType::kError);
  event.end = true;
  event.error_code = error_value.ok() ? ErrorCode::kBackendFailure : error_value.code;
  event.message = error_value.ok() ? std::string("远端会话没有产生终态事件")
                                   : error_value.message;
  const QueuePushStatus pushed = outbound.try_push(std::move(event));
  if (pushed == QueuePushStatus::kAccepted) {
    terminal_sent = true;
    return;
  }
  output_overflow = true;
}

RemoteSessionServer::RemoteSessionServer(runtime::SessionApp& app,
                                         runtime::QueuedAudioSource& input,
                                         RemoteSessionServerConfig config)
    : impl_(std::make_unique<Impl>(app, input, EffectiveServerConfig(std::move(config)))) {}

RemoteSessionServer::~RemoteSessionServer() {
  request_stop();
  if (impl_ != nullptr && impl_->worker.joinable()) {
    impl_->worker.join();
  }
}

domain::OperationResult RemoteSessionServer::bind() {
  return impl_->Bind();
}

domain::OperationResult RemoteSessionServer::serve() {
  return impl_->Serve();
}

void RemoteSessionServer::request_stop() noexcept {
  if (impl_ != nullptr) {
    impl_->RequestStop();
  }
}

std::string RemoteSessionServer::bound_endpoint() const {
  if (impl_ == nullptr) {
    return std::string();
  }
  return impl_->endpoint;
}

RemoteSessionServerStats RemoteSessionServer::stats() const {
  if (impl_ == nullptr) {
    return RemoteSessionServerStats{};
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  RemoteSessionServerStats snapshot = impl_->stats;
  const auto queue_stats = impl_->outbound.stats();
  snapshot.output_queue_capacity = queue_stats.capacity;
  snapshot.output_queue_peak = queue_stats.peak_size;
  return snapshot;
}

}  // namespace nexweave::transport

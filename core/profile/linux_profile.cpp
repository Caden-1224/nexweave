#include "linux_profile.hpp"

#include <limits>
#include <utility>

namespace nexweave::app {

namespace {

using domain::ErrorCode;
using domain::OperationResult;

domain::Error MakeError(ErrorCode code, std::string message) {
  return OperationResult::failure(code, std::move(message)).error;
}

}  // namespace

const char* to_string(LinuxProfileState state) noexcept {
  switch (state) {
    case LinuxProfileState::kIdle:
      return "idle";
    case LinuxProfileState::kStarting:
      return "starting";
    case LinuxProfileState::kReady:
      return "ready";
    case LinuxProfileState::kStreaming:
      return "streaming";
    case LinuxProfileState::kStopping:
      return "stopping";
    case LinuxProfileState::kUnavailable:
      return "unavailable";
  }
  return "";
}

domain::OperationResult validate_linux_profile_config(const LinuxProfileConfig& config) {
  if (config.stream_id.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "Linux profile 输入流标识不能为空");
  }
  const OperationResult proxy_valid =
      transport::validate_remote_session_proxy_config(config.proxy);
  if (!proxy_valid.ok()) {
    return proxy_valid;
  }
  if (config.first_generation == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "Linux profile 起始代际必须为正");
  }
  if (config.first_generation == std::numeric_limits<std::uint64_t>::max()) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "Linux profile 起始代际不能取到回绕保留值");
  }
  return OperationResult::success();
}

LinuxProfile::LinuxProfile(LinuxProfileConfig config)
    : config_(std::move(config)), proxy_(config_.proxy) {
  config_error_ = validate_linux_profile_config(config_).error;
  next_generation_ = config_.first_generation;
}

LinuxProfile::~LinuxProfile() {
  // stop() 是 noexcept 且有界；销毁前主动走一次，避免把清理结果完全交给代理析构。
  (void)stop();
}

domain::OperationResult LinuxProfile::start() {
  if (!config_error_.ok()) {
    return OperationResult{config_error_};
  }
  if (state_ == LinuxProfileState::kUnavailable) {
    return OperationResult::failure(ErrorCode::kBackendFailure,
                                    "Linux profile 已经不可用，必须先完成清理");
  }
  if (state_ != LinuxProfileState::kIdle) {
    return OperationResult::failure(ErrorCode::kBusy, "Linux profile 已经在运行或正在停止");
  }
  if (next_generation_ == 0 ||
      next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return OperationResult::failure(ErrorCode::kBackendFailure,
                                    "Linux profile 代际已经耗尽");
  }

  ++start_attempts_;
  state_ = LinuxProfileState::kStarting;
  OperationResult started;
  try {
    started = proxy_.start();
  } catch (...) {
    started = OperationResult::failure(ErrorCode::kBackendFailure,
                                       "启动 Linux profile 子进程时发生异常");
  }
  if (!started.ok()) {
    ++start_failures_;
    last_start_error_ = started.error;
    last_error_ = started.error;

    // 启动失败可能发生在 fork/exec、端点文件读取或 ready 握手之后。代理内部已经做了
    // 分阶段回滚，这里再调用一次 stop() 作为统一兜底；两次清理都必须幂等。
    const OperationResult rolled_back = proxy_.stop();
    if (!rolled_back.ok()) {
      state_ = LinuxProfileState::kUnavailable;
      last_error_ = rolled_back.error;
      return rolled_back;
    }
    state_ = LinuxProfileState::kIdle;
    return started;
  }

  ++starts_succeeded_;
  generation_ = next_generation_;
  ++next_generation_;
  stream_started_ = false;
  input_finished_ = false;
  cancel_requested_ = false;
  terminal_delivered_ = false;
  state_ = LinuxProfileState::kReady;
  last_error_ = domain::Error{};
  return OperationResult::success();
}

domain::OperationResult LinuxProfile::stop() noexcept {
  ++stop_requests_;
  if (state_ == LinuxProfileState::kIdle && !proxy_.running()) {
    ++stops_succeeded_;
    return OperationResult::success();
  }

  // 先进入停止态：后续 push_frame()/start_stream() 会立即拒绝新工作，不再把输入排到
  // 一个正在取消的进程后面。随后丢弃已经到达但尚未取走的输出，使旧数据不会在重启后
  // 被再次交付；丢弃计数由代理账目保留。
  state_ = LinuxProfileState::kStopping;
  try {
    (void)proxy_.discard_output_events();
    const OperationResult stopped = proxy_.stop();
    if (!stopped.ok()) {
      ++stop_failures_;
      state_ = LinuxProfileState::kUnavailable;
      last_error_ = stopped.error;
      return stopped;
    }
  } catch (...) {
    ++stop_failures_;
    state_ = LinuxProfileState::kUnavailable;
    last_error_ = MakeError(ErrorCode::kBackendFailure, "停止 Linux profile 时发生异常");
    return OperationResult{last_error_};
  }

  ++stops_succeeded_;
  state_ = LinuxProfileState::kIdle;
  stream_started_ = false;
  input_finished_ = false;
  cancel_requested_ = false;
  terminal_delivered_ = false;
  generation_ = 0;
  last_error_ = domain::Error{};
  return OperationResult::success();
}

domain::OperationResult LinuxProfile::restart() {
  const OperationResult stopped = stop();
  if (!stopped.ok()) {
    return stopped;
  }
  const OperationResult started = start();
  if (started.ok()) {
    ++restarts_succeeded_;
  }
  return started;
}

domain::OperationResult LinuxProfile::start_stream() {
  if (!config_error_.ok()) {
    return OperationResult{config_error_};
  }
  if (state_ != LinuxProfileState::kReady || stream_started_) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                    "Linux profile 尚未就绪或当前轮输入流已经开始");
  }

  const OperationResult started =
      proxy_.queue_start_stream(config_.stream_id, generation_);
  if (!started.ok()) {
    last_error_ = started.error;
    return started;
  }

  stream_started_ = true;
  input_finished_ = false;
  cancel_requested_ = false;
  terminal_delivered_ = false;
  state_ = LinuxProfileState::kStreaming;
  ++streams_started_;
  return OperationResult::success();
}

domain::OperationResult LinuxProfile::push_frame(const domain::AudioFrame& frame) {
  if (state_ != LinuxProfileState::kStreaming || !stream_started_ ||
      input_finished_ || cancel_requested_ || terminal_delivered_) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                    "Linux profile 当前轮输入流不能继续推送帧");
  }

  const OperationResult queued = proxy_.queue_frame(frame);
  if (!queued.ok()) {
    last_error_ = queued.error;
    return queued;
  }
  ++frames_queued_;
  return OperationResult::success();
}

domain::OperationResult LinuxProfile::finish_stream(
    std::size_t valid_samples, std::optional<domain::AudioFrame> tail) {
  if (state_ != LinuxProfileState::kStreaming || !stream_started_ ||
      input_finished_ || cancel_requested_ || terminal_delivered_) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                    "Linux profile 当前轮输入流不能结束");
  }

  const OperationResult ended =
      proxy_.queue_end_stream(valid_samples, std::move(tail));
  if (!ended.ok()) {
    last_error_ = ended.error;
    return ended;
  }
  input_finished_ = true;
  return OperationResult::success();
}

domain::OperationResult LinuxProfile::cancel_stream() {
  if (state_ != LinuxProfileState::kStreaming || !stream_started_) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "Linux profile 当前轮输入流尚未开始");
  }
  if (terminal_delivered_) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                    "Linux profile 当前轮已经交付终态");
  }
  if (cancel_requested_) {
    return OperationResult::success();
  }

  const OperationResult cancelled = proxy_.queue_cancel_stream();
  if (!cancelled.ok()) {
    last_error_ = cancelled.error;
    return cancelled;
  }
  cancel_requested_ = true;
  return OperationResult::success();
}

std::size_t LinuxProfile::poll_events(std::vector<protocol::DataEvent>& out,
                                      std::size_t max_events,
                                      std::chrono::milliseconds timeout) {
  if (max_events == 0) {
    return 0;
  }
  if (state_ != LinuxProfileState::kReady && state_ != LinuxProfileState::kStreaming) {
    return 0;
  }

  // 终态交付后只做零等待排空：把 socket 和软件队列中仍在途的旧事件取回并拒绝，
  // 而不是让调用方传入的长 timeout 在一个已经没有合法输出的流上阻塞。
  if (terminal_delivered_) {
    std::vector<protocol::DataEvent> late;
    const std::size_t late_count =
        proxy_.receive_events(late, max_events, std::chrono::milliseconds::zero());
    late_events_filtered_ += late_count;
    (void)proxy_.discard_output_events();
    const domain::Error proxy_error = proxy_.last_error();
    if (!proxy_error.ok()) {
      last_error_ = proxy_error;
    }
    return 0;
  }

  std::vector<protocol::DataEvent> batch;
  (void)proxy_.receive_events(batch, max_events, timeout);
  std::size_t delivered = 0;
  for (protocol::DataEvent& event : batch) {
    if (terminal_delivered_) {
      ++late_events_filtered_;
      continue;
    }

    const bool terminal = event.end;
    out.push_back(std::move(event));
    ++delivered;
    if (terminal) {
      terminal_delivered_ = true;
      ++terminal_events_;
      // 同一批里排在终态之后的事件已经在循环中继续被过滤；输出队列里尚未取走的
      // 旧事件也在这里清掉，避免下一次 start 或调用方稍后读取时重新出现。
      (void)proxy_.discard_output_events();
    }
  }

  const domain::Error proxy_error = proxy_.last_error();
  if (!proxy_error.ok()) {
    last_error_ = proxy_error;
  }
  return delivered;
}

domain::Error LinuxProfile::last_error() const noexcept {
  if (!last_error_.ok()) {
    return last_error_;
  }
  return proxy_.last_error();
}

bool LinuxProfile::running() const noexcept {
  return state_ == LinuxProfileState::kStarting ||
         state_ == LinuxProfileState::kReady ||
         state_ == LinuxProfileState::kStreaming;
}

LinuxProfileStatus LinuxProfile::status() const {
  LinuxProfileStatus snapshot;
  snapshot.state = state_;
  snapshot.process_running = proxy_.running();
  snapshot.stream_started = stream_started_;
  snapshot.input_finished = input_finished_;
  snapshot.cancel_requested = cancel_requested_;
  snapshot.terminal_delivered = terminal_delivered_;
  snapshot.generation = generation_;
  snapshot.start_attempts = start_attempts_;
  snapshot.starts_succeeded = starts_succeeded_;
  snapshot.start_failures = start_failures_;
  snapshot.stop_requests = stop_requests_;
  snapshot.stops_succeeded = stops_succeeded_;
  snapshot.stop_failures = stop_failures_;
  snapshot.restarts_succeeded = restarts_succeeded_;
  snapshot.streams_started = streams_started_;
  snapshot.frames_queued = frames_queued_;
  snapshot.terminal_events = terminal_events_;
  snapshot.late_events_filtered = late_events_filtered_;
  snapshot.last_start_error = last_start_error_;
  snapshot.last_error = last_error();
  snapshot.proxy = proxy_.stats();
  return snapshot;
}

}  // namespace nexweave::app

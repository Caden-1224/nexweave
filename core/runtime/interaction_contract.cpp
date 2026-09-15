#include "interaction_contract.hpp"

#include <algorithm>
#include <limits>

namespace nexweave::runtime {
namespace {

constexpr std::size_t kFrameSamples = 320;

bool valid_kind(InputEventKind kind) {
  switch (kind) {
    case InputEventKind::kStart:
    case InputEventKind::kFrame:
    case InputEventKind::kEnd:
    case InputEventKind::kCancel:
      return true;
  }
  return false;
}


bool same_frame(const domain::AudioFrame& frame) {
  return domain::validate_audio_frame(frame).ok() &&
         frame.samples.size() == kFrameSamples;
}

}  // namespace

domain::OperationResult validate_input_event(const InputStreamEvent& event) {
  if (event.version != 1 || event.stream_id.empty() || !valid_kind(event.kind)) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }
  if (event.kind == InputEventKind::kFrame) {
    if (!event.frame.has_value() || event.valid_samples != kFrameSamples ||
        !same_frame(*event.frame)) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
  } else if (event.kind == InputEventKind::kEnd) {
    if (event.valid_samples > kFrameSamples ||
        (event.valid_samples > 0 &&
         (!event.frame.has_value() || !same_frame(*event.frame)))) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
  } else if (event.frame.has_value() || event.valid_samples != 0) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }
  return domain::OperationResult::success();
}

domain::Result<std::vector<PaddedAudioFrame>> pad_audio_samples(
    const std::vector<std::int16_t>& samples) {
  std::vector<PaddedAudioFrame> output;
  if (samples.empty()) {
    return domain::Result<std::vector<PaddedAudioFrame>>::success(std::move(output));
  }
  for (std::size_t offset = 0; offset < samples.size(); offset += kFrameSamples) {
    const std::size_t remaining = samples.size() - offset;
    const std::size_t valid_samples = std::min(remaining, kFrameSamples);
    std::vector<std::int16_t> padded(kFrameSamples, 0);
    std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(offset),
                valid_samples, padded.begin());
    auto frame = domain::AudioFrame::from_samples(std::move(padded));
    if (!frame.ok()) {
      return domain::Result<std::vector<PaddedAudioFrame>>::failure(domain::ErrorCode::kInvalidInput,
                                                                     "invalid padded audio frame");
    }
    output.push_back(PaddedAudioFrame{std::move(frame.value), valid_samples});
  }
  return domain::Result<std::vector<PaddedAudioFrame>>::success(std::move(output));
}

// 提交标记的唯一入口：轨迹、末标记与观察者通知三件事只在这里发生。
// 顺序理由：先把状态改完再通知。观察者若在回调里（非法规地）回头读夹具，看到的必须是
// 已经包含本次标记的状态；反过来先通知再改状态，会让回调观察到"标记还没发生"的假象。
// 观察者是借用指针且约定不抛异常，因此这里不需要异常保护；空指针时不产生任何额外分支。
void InteractionContractFixture::commit(ActivityMarker marker) {
  last_marker_ = marker;
  trace_.push_back(marker);
  if (marker_observer_ != nullptr) {
    marker_observer_->on_marker(marker, generation_);
  }
}

void InteractionContractFixture::set_marker_observer(IMarkerObserver* observer) noexcept {
  marker_observer_ = observer;
}

domain::OperationResult InteractionContractFixture::start_stream(
    const std::string& stream_id) {
  if (stream_id.empty() || stream_started_) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }
  stream_id_ = stream_id;
  stream_started_ = true;
  stream_ended_ = false;
  next_sequence_ = 0;
  return domain::OperationResult::success();
}

domain::OperationResult InteractionContractFixture::push_frame(
    const InputStreamEvent& event) {
  if (!validate_input_event(event).ok() || event.kind != InputEventKind::kFrame ||
      !stream_started_ || stream_ended_ || event.stream_id != stream_id_ ||
      event.generation != generation_ || event.sequence != next_sequence_) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }
  ++next_sequence_;
  return domain::OperationResult::success();
}

domain::OperationResult InteractionContractFixture::end_stream(
    const InputStreamEvent& event) {
  if (!validate_input_event(event).ok() || event.kind != InputEventKind::kEnd ||
      !stream_started_ || stream_ended_ || event.stream_id != stream_id_ ||
      event.generation != generation_ || event.sequence != next_sequence_) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }
  stream_ended_ = true;
  ++next_sequence_;
  return domain::OperationResult::success();
}

domain::Result<std::uint64_t> InteractionContractFixture::begin_generation() {
  if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return domain::Result<std::uint64_t>::failure(domain::ErrorCode::kInvalidInput);
  }
  if (generation_started_ && !terminal_) {
    // 旧输出封锁先于新代际的起点提交：它描述的是"上一轮的结果已经不可能再被提交"。
    commit(ActivityMarker::kOldOutputBlocked);
  }
  ++generation_;
  generation_started_ = true;
  generation_done_ = false;
  synthesis_done_ = false;
  playback_started_ = false;
  playback_done_ = false;
  cancelled_ = false;
  terminal_ = false;
  commit(ActivityMarker::kGenerationStarted);
  return domain::Result<std::uint64_t>::success(generation_);
}

domain::OperationResult InteractionContractFixture::start_playback(
    std::uint64_t generation) {
  if (!generation_started_ || generation != generation_ || cancelled_ || terminal_) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }
  playback_started_ = true;
  commit(ActivityMarker::kPlaybackStarted);
  return domain::OperationResult::success();
}

domain::OperationResult InteractionContractFixture::mark_generation_done(
    std::uint64_t generation) {
  if (!generation_started_ || generation != generation_ || cancelled_ || terminal_) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }
  generation_done_ = true;
  commit(ActivityMarker::kGenerationDone);
  return domain::OperationResult::success();
}

domain::OperationResult InteractionContractFixture::mark_synthesis_done(
    std::uint64_t generation) {
  if (!generation_started_ || generation != generation_ || cancelled_ || terminal_) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }
  synthesis_done_ = true;
  commit(ActivityMarker::kSynthesisDone);
  return domain::OperationResult::success();
}

domain::OperationResult InteractionContractFixture::mark_playback_done(
    std::uint64_t generation) {
  if (!generation_started_ || generation != generation_ || cancelled_ || terminal_) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }
  if (!playback_started_) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }
  playback_done_ = true;
  commit(ActivityMarker::kPlaybackDone);
  if (generation_done_ && synthesis_done_) {
    terminal_ = true;
    // 成功终态是本地标记提交的结果，不是一次独立的调用：把它和上面那个标记之间的状态
    // 更新放在同一处，终端就不会在缺少三个 done 的情况下被提交。
    commit(ActivityMarker::kTerminalSucceeded);
  }
  return domain::OperationResult::success();
}

domain::OperationResult InteractionContractFixture::cancel(
    std::uint64_t generation) {
  if (!generation_started_ || generation != generation_ || terminal_) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }
  if (cancelled_) {
    return domain::OperationResult::success();
  }
  cancelled_ = true;
  // 五个阶段在同一个线性化点上提交：受理、封锁旧输出、执行退出、播放清理、取消终态。
  // 本夹具的清理是同步的（没有等待），因此它们的观测时刻相同；真实设备需要有限等待时，
  // 阶段之间的时间差才会出现，而顺序不变量保持不变。
  commit(ActivityMarker::kCancelAccepted);
  commit(ActivityMarker::kOldOutputBlocked);
  commit(ActivityMarker::kExecutionExited);
  commit(ActivityMarker::kPlaybackCleared);
  terminal_ = true;
  commit(ActivityMarker::kTerminalCancelled);
  return domain::OperationResult::success();
}

ActivityMarker InteractionContractFixture::last_marker() const noexcept {
  return last_marker_;
}

std::vector<ActivityMarker> InteractionContractFixture::trace() const {
  return trace_;
}

bool InteractionContractFixture::terminal() const noexcept {
  return terminal_;
}

std::uint64_t InteractionContractFixture::generation() const noexcept {
  return generation_;
}

}  // namespace nexweave::runtime

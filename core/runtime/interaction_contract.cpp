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
    trace_.push_back(ActivityMarker::kOldOutputBlocked);
    last_marker_ = ActivityMarker::kOldOutputBlocked;
  }
  ++generation_;
  generation_started_ = true;
  generation_done_ = false;
  synthesis_done_ = false;
  playback_started_ = false;
  playback_done_ = false;
  cancelled_ = false;
  terminal_ = false;
  trace_.push_back(ActivityMarker::kGenerationStarted);
  last_marker_ = ActivityMarker::kGenerationStarted;
  return domain::Result<std::uint64_t>::success(generation_);
}

domain::OperationResult InteractionContractFixture::start_playback(
    std::uint64_t generation) {
  if (!generation_started_ || generation != generation_ || cancelled_ || terminal_) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }
  playback_started_ = true;
  trace_.push_back(ActivityMarker::kPlaybackStarted);
  last_marker_ = ActivityMarker::kPlaybackStarted;
  return domain::OperationResult::success();
}

domain::OperationResult InteractionContractFixture::mark_generation_done(
    std::uint64_t generation) {
  if (!generation_started_ || generation != generation_ || cancelled_ || terminal_) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }
  generation_done_ = true;
  trace_.push_back(ActivityMarker::kGenerationDone);
  last_marker_ = ActivityMarker::kGenerationDone;
  return domain::OperationResult::success();
}

domain::OperationResult InteractionContractFixture::mark_synthesis_done(
    std::uint64_t generation) {
  if (!generation_started_ || generation != generation_ || cancelled_ || terminal_) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }
  synthesis_done_ = true;
  trace_.push_back(ActivityMarker::kSynthesisDone);
  last_marker_ = ActivityMarker::kSynthesisDone;
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
  trace_.push_back(ActivityMarker::kPlaybackDone);
  last_marker_ = ActivityMarker::kPlaybackDone;
  if (generation_done_ && synthesis_done_) {
    terminal_ = true;
    trace_.push_back(ActivityMarker::kTerminalSucceeded);
    last_marker_ = ActivityMarker::kTerminalSucceeded;
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
  trace_.push_back(ActivityMarker::kCancelAccepted);
  trace_.push_back(ActivityMarker::kOldOutputBlocked);
  trace_.push_back(ActivityMarker::kExecutionExited);
  trace_.push_back(ActivityMarker::kPlaybackCleared);
  trace_.push_back(ActivityMarker::kTerminalCancelled);
  last_marker_ = ActivityMarker::kTerminalCancelled;
  terminal_ = true;
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

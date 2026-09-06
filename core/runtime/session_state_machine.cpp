#include "session_state_machine.hpp"
#include <limits>
namespace nexweave::runtime { namespace {
const char* state_to_text(SessionStateMachine::State s) {
  switch (s) {
    case SessionStateMachine::State::kIdle: return "idle";
    case SessionStateMachine::State::kListening: return "listening";
    case SessionStateMachine::State::kRouting: return "routing";
    case SessionStateMachine::State::kThinking: return "thinking";
    case SessionStateMachine::State::kSpeaking: return "speaking";
    case SessionStateMachine::State::kCancelling: return "cancelling";
  }
  return "unknown";
}
const char* event_name(SessionStateMachine::Event e) {
  switch (e) {
    case SessionStateMachine::Event::kAudioStart: return "audio_start";
    case SessionStateMachine::Event::kAsrFinal: return "asr_final";
    case SessionStateMachine::Event::kRouteL0L1: return "route_l0_l1";
    case SessionStateMachine::Event::kRouteL2L3: return "route_l2_l3";
    case SessionStateMachine::Event::kLlmDone: return "llm_done";
    case SessionStateMachine::Event::kTtsDone: return "tts_done";
    case SessionStateMachine::Event::kCancel: return "cancel";
    case SessionStateMachine::Event::kCancelComplete: return "cancel_complete";
  }
  return "unknown";
}
}}
namespace nexweave::runtime {
domain::OperationResult SessionStateMachine::dispatch(Event e) {
  return dispatch(e, generation());
}

domain::OperationResult SessionStateMachine::dispatch(Event e, std::uint64_t supplied_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (supplied_generation != generation_) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
      "拒绝过时代际事件");
  }
  State next = state_;
  bool valid = false;
  switch (state_) {
    case State::kIdle:
      valid = e == Event::kAudioStart;
      if (valid) next = State::kListening;
      break;
    case State::kListening:
      valid = e == Event::kAsrFinal || e == Event::kCancel;
      if (e == Event::kAsrFinal) next = State::kRouting;
      if (e == Event::kCancel) next = State::kCancelling;
      break;
    case State::kRouting:
      valid = e == Event::kRouteL0L1 || e == Event::kRouteL2L3 || e == Event::kCancel;
      if (e == Event::kRouteL0L1) next = State::kSpeaking;
      if (e == Event::kRouteL2L3) next = State::kThinking;
      if (e == Event::kCancel) next = State::kCancelling;
      break;
    case State::kThinking:
      valid = e == Event::kLlmDone || e == Event::kCancel;
      if (e == Event::kLlmDone) next = State::kSpeaking;
      if (e == Event::kCancel) next = State::kCancelling;
      break;
    case State::kSpeaking:
      valid = e == Event::kTtsDone || e == Event::kCancel;
      if (e == Event::kTtsDone) next = State::kIdle;
      if (e == Event::kCancel) next = State::kCancelling;
      break;
    case State::kCancelling:
      valid = e == Event::kCancelComplete;
      if (valid) next = State::kIdle;
      break;
  }
  if (!valid) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
      std::string("非法 Session 状态迁移: ") + state_to_text(state_) + " + " + event_name(e));
  }
  if ((e == Event::kAudioStart || e == Event::kCancel) &&
      generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
      "generation 已耗尽，拒绝开启新代际");
  }
  // 轨迹先写入，随后才提交无抛出的状态/代际赋值；若分配失败，三者保持原值。
  // 这样取消窗口不会出现“代际已变但状态未变”的半提交状态。
  const auto transition = std::string(state_to_text(state_)) + "--" + event_name(e) + "-->" + state_to_text(next);
  trace_.push_back(transition);
  if (e == Event::kAudioStart || e == Event::kCancel) {
    ++generation_;
  }
  state_ = next;
  return domain::OperationResult::success();
}
SessionStateMachine::State SessionStateMachine::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::uint64_t SessionStateMachine::generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}

const char* SessionStateMachine::state_name() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_to_text(state_);
}

std::vector<std::string> SessionStateMachine::trace() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return trace_;
}

void SessionStateMachine::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = State::kIdle;
  trace_.clear();
}
}

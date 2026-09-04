#include "session_state_machine.hpp"
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
  std::lock_guard<std::mutex> lock(mutex_);
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
  trace_.push_back(std::string(state_to_text(state_)) + "--" + event_name(e) + "-->" + state_to_text(next));
  state_ = next;
  return domain::OperationResult::success();
}
SessionStateMachine::State SessionStateMachine::state()const{std::lock_guard<std::mutex>l(mutex_);return state_;}
const char* SessionStateMachine::state_name()const{std::lock_guard<std::mutex>l(mutex_);return state_to_text(state_);}
std::vector<std::string> SessionStateMachine::trace()const{std::lock_guard<std::mutex>l(mutex_);return trace_;}
void SessionStateMachine::reset(){std::lock_guard<std::mutex>l(mutex_);state_=State::kIdle;trace_.clear();}
}

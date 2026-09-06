#include "fake_asr.hpp"

#include <algorithm>
#include <utility>

namespace nexweave::backend {

FakeAsr::FakeAsr(std::vector<std::string> hypotheses)
    : hypotheses_(std::move(hypotheses)) {
}

domain::OperationResult FakeAsr::set_callback(capability::TextEventCallback callback) {
  // 所有检查先于替换，失败注册不能清除已有轮次或取消状态。
  if (!callback || hypotheses_.empty() ||
      std::any_of(hypotheses_.begin(), hypotheses_.end(),
                  [](const std::string& text) { return text.empty(); })) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }
  callback_ = std::move(callback);
  next_hypothesis_ = 0;
  cancelled_ = false;
  return domain::OperationResult::success();
}

domain::OperationResult FakeAsr::feed(const domain::AudioFrame& frame, bool is_last) {
  // 取消优先于输入错误，否则坏帧可能掩盖本轮已经失效这一事实。
  if (cancelled_) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }
  if (!callback_ || !domain::validate_audio_frame(frame).ok()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }

  // 先复制本次文本再进入交付区：分配失败尚未产生事件，游标可以保持原位重试。
  const capability::TextEvent event{
      is_last ? capability::TextEventKind::kFinal : capability::TextEventKind::kPartial,
      is_last ? hypotheses_.back() : hypotheses_[next_hypothesis_],
      {}};
  try {
    callback_(event);
    if (is_last) {
      callback_({capability::TextEventKind::kDone, "", {}});
    }
  } catch (...) {
    // 接收方可能已经处理事件再抛出，无法回滚外部副作用；封锁本轮避免重复交付。
    // 无输出队列或后台资源需要等待，重新注册才恢复；原异常保留给调用方定位。
    cancel();
    throw;
  }

  // 回调全部成功才提交游标；终态归零，使下一帧自然开启独立新轮次。
  // 非终态饱和在最后一项，长流不会越界，也不会因计数溢出回到首项。
  if (is_last) {
    next_hypothesis_ = 0;
  } else if (next_hypothesis_ < hypotheses_.size() - 1) {
    ++next_hypothesis_;
  }
  return domain::OperationResult::success();
}

domain::OperationResult FakeAsr::cancel() noexcept {
  cancelled_ = true;
  next_hypothesis_ = 0;
  return domain::OperationResult::success();
}

}  // namespace nexweave::backend
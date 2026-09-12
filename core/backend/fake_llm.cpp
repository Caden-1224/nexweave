#include "fake_llm.hpp"

#include <utility>

namespace nexweave::backend {

FakeLlm::FakeLlm(std::vector<std::string> tokens) : tokens_(std::move(tokens)) {}

domain::OperationResult FakeLlm::set_callback(capability::TextEventCallback callback) {
  // 与 FakeAsr/FakeTts 同序：全部检查先于替换。失败注册既不能改写旧回调，也不能
  // 解除已取消轮次的封锁，否则一次坏注册会让旧轮次“复活”并重复交付。
  if (!callback) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }
  for (const auto& token : tokens_) {
    if (token.empty()) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
  }
  if (tokens_.empty()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }
  callback_ = std::move(callback);
  cancelled_.store(false);
  return domain::OperationResult::success();
}

domain::OperationResult FakeLlm::generate(const std::string& prompt) {
  // 取消优先于输入校验：取消后的坏输入必须报 kCancelled 而不是 kInvalidInput，
  // 否则坏输入会掩盖“本轮已经失效”这一更早发生的事实（与 FakeAsr/FakeTts 同序）。
  if (cancelled_.load()) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }
  if (!callback_ || prompt.empty()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }

  // 探针只报告“真实发生的事实”：开始、逐 token 交付、正常结束或失败。测试据此把
  // “首段已开始播放”与“生成尚未结束”放在同一条事件链上比较，而不需要真实时间。
  // 生成真正开始：先通知后端侧观察者，再通知编排层探针，两者都可以为空。
  if (observer_ != nullptr && !observer_->failed()) {
    observer_->on_generation_started();
  }
  if (probe_ != nullptr) {
    probe_->on_generation_started();
  }

  try {
    for (const auto& token : tokens_) {
      // 每个 token 交付前的取消检查就是并发 cancel 的线性化点。取消在此之后到达时，
      // 当前 token 仍会完成本次交付（并发窗口，无法撤回），从下一个 token 起生效。
      if (cancelled_.load()) {
        notify_failed("cancelled before token delivery");
        return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
      }
      callback_({capability::TextEventKind::kToken, token, {}});
      report_token(token);
    }
    callback_({capability::TextEventKind::kDone, "", {}});
  } catch (...) {
    // 回调抛出异常：封锁本轮并原样向上传播，避免调用方重试造成重复交付。探针额外
    // 记录“以异常失败”，使运行证据里能看到是回调路径而不是取消结束了本轮。
    notify_failed("callback threw");
    cancel();
    throw;
  }

  // 结束边界：全部 token 与 done 已交付但尚未返回时取消到达，仍收敛为 kCancelled，
  // 不能把“事件已经发完”误报为成功终态；已交付的事件不可撤回，取消只禁止开启新轮次。
  if (cancelled_.load()) {
    notify_failed("cancelled after last token");
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }

  report_completed();
  return domain::OperationResult::success();
}

domain::OperationResult FakeLlm::cancel() noexcept {
  // 原子置位即为串行与并发两种场景下的统一取消请求；无分配、不抛异常。
  // 生效窗口（至多再交付一个 token）由 generate 内的检查点决定，见头文件注释。
  cancelled_.store(true);
  return domain::OperationResult::success();
}

void FakeLlm::set_observer(capability::IGenerationObserver* observer) noexcept {
  observer_ = observer;
}

void FakeLlm::set_progress_probe(capability::IGenerationProbe* probe) noexcept {
  probe_ = probe;
}

bool FakeLlm::reporting_suppressed() const noexcept {
  // 观察者报告失败后，本轮不再向任何一端报告后续事件。
  return observer_ != nullptr && observer_->failed();
}

void FakeLlm::notify_token(const std::string& token) {
  if (reporting_suppressed()) {
    return;
  }
  if (observer_ != nullptr) {
    observer_->on_token_delivered(token);
  }
  if (probe_ != nullptr) {
    probe_->on_token_delivered(token);
  }
}

void FakeLlm::notify_completed() {
  if (reporting_suppressed()) {
    return;
  }
  if (observer_ != nullptr) {
    observer_->on_generation_completed();
  }
  if (probe_ != nullptr) {
    probe_->on_generation_completed();
  }
}

void FakeLlm::notify_failed(const std::string& message) {
  if (observer_ != nullptr) {
    observer_->on_generation_failed(message);
  }
  if (probe_ != nullptr) {
    probe_->on_generation_failed(message);
  }
}

void FakeLlm::report_token(const std::string& token) {
  notify_token(token);
}

void FakeLlm::report_completed() {
  notify_completed();
}

}  // namespace nexweave::backend

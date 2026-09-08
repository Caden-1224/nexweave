#include "fake_llm.hpp"
#include <algorithm>
#include <utility>
namespace nexweave::backend {
FakeLlm::FakeLlm(std::vector<std::string> tokens) : tokens_(std::move(tokens)) {}
domain::OperationResult FakeLlm::set_callback(capability::TextEventCallback callback) {
  if (!callback || tokens_.empty() || std::any_of(tokens_.begin(), tokens_.end(), [](const std::string& token) { return token.empty(); })) return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  callback_ = std::move(callback); cancelled_ = false; return domain::OperationResult::success();
}
domain::OperationResult FakeLlm::generate(const std::string& prompt) {
  if (cancelled_) return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  if (!callback_ || prompt.empty()) return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  try { for (const auto& token : tokens_) callback_({capability::TextEventKind::kToken, token, {}}); callback_({capability::TextEventKind::kDone, "", {}}); }
  catch (...) { cancel(); throw; }
  return domain::OperationResult::success();
}
domain::OperationResult FakeLlm::cancel() noexcept { cancelled_ = true; return domain::OperationResult::success(); }
}  // namespace nexweave::backend

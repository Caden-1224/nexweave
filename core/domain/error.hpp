// NexWeave 统一错误与终态结果契约。
#pragma once
#include <optional>
#include <string>
#include <utility>
namespace nexweave::domain {
enum class ErrorCode { kNone = 0, kInvalidInput, kMissingField, kAlreadyCompleted, kCancelled, kTimeout, kBackendFailure, kDeviceFailure };
// 调用方据此决定是否重试：超时/后端/设备失败可重试；取消必须停止当前代；输入和重复终态不可恢复。
enum class ErrorDisposition { kSuccess, kRetryable, kCancelled, kPermanent };
// Error 仅携带可复制诊断文本，不拥有线程、句柄或其他外部资源；code 决定机器语义。
struct Error { ErrorCode code = ErrorCode::kNone; std::string message; bool ok() const noexcept { return code == ErrorCode::kNone; } bool cancelled() const noexcept { return code == ErrorCode::kCancelled; } ErrorDisposition disposition() const noexcept; bool retryable() const noexcept { return disposition() == ErrorDisposition::kRetryable; } };
// 工厂不阻塞、不创建资源；协议层应始终以 code 分支，message 仅用于诊断日志。
struct OperationResult { Error error{}; bool ok() const noexcept { return error.ok(); } static OperationResult success() { return {}; } static OperationResult failure(ErrorCode code, std::string message = {}); };
template <typename T> struct Result { std::optional<T> value; Error error{}; bool ok() const noexcept { return error.ok() && value.has_value(); } static Result success(T result) { Result output; output.value.emplace(std::move(result)); return output; } static Result failure(ErrorCode code, std::string message = {}) { Result output; output.error = Error{code == ErrorCode::kNone ? ErrorCode::kInvalidInput : code, std::move(message)}; return output; } };
}  // namespace nexweave::domain

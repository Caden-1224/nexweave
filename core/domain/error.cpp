#include "error.hpp"
namespace nexweave::domain {
ErrorDisposition Error::disposition() const noexcept { switch (code) { case ErrorCode::kNone: return ErrorDisposition::kSuccess; case ErrorCode::kCancelled: return ErrorDisposition::kCancelled; case ErrorCode::kTimeout: case ErrorCode::kBackendFailure: case ErrorCode::kDeviceFailure: return ErrorDisposition::kRetryable; case ErrorCode::kInvalidInput: case ErrorCode::kMissingField: case ErrorCode::kAlreadyCompleted: return ErrorDisposition::kPermanent; } return ErrorDisposition::kPermanent; }
OperationResult OperationResult::failure(ErrorCode code, std::string message) {
  if (code == ErrorCode::kNone) code = ErrorCode::kInvalidInput;
  return OperationResult{Error{code, std::move(message)}};
}
}  // namespace nexweave::domain

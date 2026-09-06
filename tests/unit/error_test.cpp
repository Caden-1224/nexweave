#include "error.hpp"

#include <cstdio>
namespace {
int failures = 0;
#define CHECK(value)                     \
  do {                                   \
    if (!(value)) {                      \
      std::printf("FAIL: %s\n", #value); \
      ++failures;                        \
    }                                    \
  } while (0)
// 稳定错误分类区分重试、取消与永久失败；分类本身不执行重试。
void TestErrors() {
  using namespace nexweave::domain;
  CHECK(Error{}.ok());
  CHECK((Error{ErrorCode::kTimeout, {}}.retryable()));
  CHECK((Error{ErrorCode::kBackendFailure, {}}.retryable()));
  CHECK((Error{ErrorCode::kCancelled, {}}.cancelled()));
  CHECK((!Error{ErrorCode::kInvalidInput, {}}.retryable()));
}
// 成功必须携带值，失败不得携带可消费值；错误码在结果工厂中保持有效。
void TestResults() {
  using namespace nexweave::domain;
  CHECK(OperationResult::success().ok());
  const auto failure = OperationResult::failure(ErrorCode::kMissingField, "request_id");
  CHECK(!failure.ok() && failure.error.code == ErrorCode::kMissingField);
  const auto value = Result<int>::success(42);
  CHECK(value.ok() && *value.value == 42);
  const auto bad = Result<int>::failure(ErrorCode::kDeviceFailure);
  CHECK(!bad.ok() && !bad.value.has_value() && bad.error.retryable());
}
}  // namespace
int main() {
  TestErrors();
  TestResults();
  return failures == 0 ? 0 : 1;
}

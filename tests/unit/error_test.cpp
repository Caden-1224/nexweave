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
// busy 必须落在“可重试”一侧：它表达的是资源被正常占用，稍后重试是正确用法；
// 若被归为永久失败，调用方会把一次正常的单活跃拒绝当成不可恢复故障。
void TestErrors() {
  using namespace nexweave::domain;
  CHECK(Error{}.ok());
  CHECK((Error{ErrorCode::kTimeout, {}}.retryable()));
  CHECK((Error{ErrorCode::kBackendFailure, {}}.retryable()));
  CHECK((Error{ErrorCode::kBusy, {}}.retryable()));
  CHECK((!Error{ErrorCode::kBusy, {}}.cancelled()));
  CHECK((Error{ErrorCode::kCancelled, {}}.cancelled()));
  CHECK((!Error{ErrorCode::kInvalidInput, {}}.retryable()));
}
// busy 是线协议的一部分：它必须能通过合法性校验并在结果工厂中原样保留，
// 否则协议适配层会把它当成未知码，调用方也就拿不到结构化的忙碌语义。
void TestBusyCodeIsValidAndPreserved() {
  using namespace nexweave::domain;
  CHECK(is_valid_error_code(ErrorCode::kBusy));
  const auto failure = OperationResult::failure(ErrorCode::kBusy, "已经有活跃会话");
  CHECK(!failure.ok() && failure.error.code == ErrorCode::kBusy);
  const auto value = Result<int>::failure(ErrorCode::kBusy);
  CHECK(!value.ok() && value.error.code == ErrorCode::kBusy);
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
  TestBusyCodeIsValidAndPreserved();
  TestResults();
  return failures == 0 ? 0 : 1;
}

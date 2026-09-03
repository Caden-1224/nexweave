#include "error.hpp"
#include <cstdio>
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { std::printf("FAIL: %s\n", #value); ++failures; } } while (0)
void TestErrors() { using namespace nexweave::domain; CHECK(Error{}.ok()); CHECK((Error{ErrorCode::kTimeout, {}}.retryable())); CHECK((Error{ErrorCode::kBackendFailure, {}}.retryable())); CHECK((Error{ErrorCode::kCancelled, {}}.cancelled())); CHECK((!Error{ErrorCode::kInvalidInput, {}}.retryable())); }
void TestResults() { using namespace nexweave::domain; CHECK(OperationResult::success().ok()); const auto failure = OperationResult::failure(ErrorCode::kMissingField, "request_id"); CHECK(!failure.ok() && failure.error.code == ErrorCode::kMissingField); const auto value = Result<int>::success(42); CHECK(value.ok() && *value.value == 42); const auto bad = Result<int>::failure(ErrorCode::kDeviceFailure); CHECK(!bad.ok() && !bad.value.has_value() && bad.error.retryable()); }
}
int main() { TestErrors(); TestResults(); return failures == 0 ? 0 : 1; }

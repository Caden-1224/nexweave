// 控制契约测试：真实编解码保留关联，重复请求不改变语义，预算边界拒绝超时。
// 时间由夹具显式传入，避免 sleep 和时钟调度造成不确定性；不创建线程或外部资源。
#include "../../core/protocol/control_rpc.hpp"

#include <nlohmann/json.hpp>

#include "../test_support.hpp"

namespace {
namespace p = nexweave::protocol;
using nexweave::domain::ErrorCode;
using nexweave::domain::OperationResult;
using namespace std::chrono_literals;

p::ControlRequest Request() {
  p::ControlRequest request;
  request.request_id = "req-1";
  request.operation = "start";
  request.work_id = "w-1";
  request.session_id = "s-1";
  request.generation = 2;
  request.deadline = 100ms;
  return request;
}

void TestRoundTripAndMalformedResponse() {
  const auto request = Request();
  const auto encoded = p::encode_request(request);
  CHECK(encoded.ok());
  const auto decoded = p::decode_request(*encoded.value);
  CHECK(decoded.ok());
  CHECK(p::validate_retry(request, *decoded.value).ok());
  CHECK(!p::decode_request("{\"version\":1}").ok());

  p::ControlResponse response;
  response.request_id = request.request_id;
  response.result = OperationResult::failure(ErrorCode::kBackendFailure, "后端失败");
  const auto serialized = p::encode_response(response);
  CHECK(serialized.ok());
  const auto parsed = p::decode_response(*serialized.value);
  CHECK(parsed.ok());
  CHECK(parsed.value->result.error.code == ErrorCode::kBackendFailure);
  CHECK(parsed.value->result.error.message == "后端失败");

  // 失败场景：未知版本和 ok/错误码矛盾必须拒绝，不能把失败响应误认为成功。
  auto json = nlohmann::json::parse(*serialized.value);
  json["ok"] = true;
  CHECK(!p::decode_response(json.dump()).ok());
  json["ok"] = false;
  json["version"] = 2;
  CHECK(!p::decode_response(json.dump()).ok());
}

void TestRetryIdentityAndReplay() {
  const auto request = Request();
  p::ControlResponse completed;
  completed.request_id = request.request_id;
  const auto replayed = p::replay_response(request, request, completed);
  CHECK(replayed.ok());
  CHECK(replayed.value->replayed);
  CHECK(replayed.value->result.ok());
  CHECK(!completed.replayed);

  completed.result = OperationResult::failure(ErrorCode::kCancelled, "已取消");
  const auto cancelled = p::replay_response(request, request, completed);
  CHECK(cancelled.ok());
  CHECK(cancelled.value->result.error.code == ErrorCode::kCancelled);
  CHECK(cancelled.value->result.error.message == "已取消");

  // 每个字段均参与重试身份，不能通过相同 request_id 偷换操作、预算、任务或代际。
  for (int field = 0; field < 7; ++field) {
    auto changed = request;
    switch (field) {
      case 0:
        changed.version = 2;
        break;
      case 1:
        changed.request_id = "req-2";
        break;
      case 2:
        changed.operation = "cancel";
        break;
      case 3:
        changed.work_id = "w-2";
        break;
      case 4:
        changed.session_id = "s-2";
        break;
      case 5:
        ++changed.generation;
        break;
      case 6:
        changed.deadline = 101ms;
        break;
    }
    CHECK(!p::validate_retry(request, changed).ok());
    CHECK(!p::replay_response(request, changed, completed).ok());
  }
  // 请求本身非法时保留字段/预算校验错误，不把它误标为两个合法请求的冲突。
  auto missing = request;
  missing.request_id.clear();
  CHECK(p::validate_retry(request, missing).error.code == ErrorCode::kMissingField);
  auto expired = request;
  expired.deadline = 0ms;
  CHECK(p::replay_response(request, expired, completed).error.code == ErrorCode::kTimeout);
  completed.request_id = "req-other";
  CHECK(!p::replay_response(request, request, completed).ok());
}

void TestDeadlineBoundaries() {
  const auto request = Request();
  CHECK(p::check_deadline(request, 0ms).ok());
  CHECK(p::check_deadline(request, 99ms).ok());
  CHECK(p::check_deadline(request, 100ms).error.code == ErrorCode::kTimeout);
  CHECK(p::check_deadline(request, 101ms).error.code == ErrorCode::kTimeout);
  CHECK(p::check_deadline(request, -1ms).error.code == ErrorCode::kInvalidInput);
  auto invalid = request;
  invalid.deadline = 0ms;
  CHECK(p::validate_request(invalid).error.code == ErrorCode::kTimeout);
  invalid = request;
  invalid.operation = "unknown";
  CHECK(p::validate_request(invalid).error.code == ErrorCode::kInvalidInput);
  // 重试仍以原始接收点计时，纯函数的重复调用不会延长预算。
  CHECK(p::validate_retry(request, request).ok());
  CHECK(p::check_deadline(request, 100ms).error.code == ErrorCode::kTimeout);
}
}  // namespace

int main() {
  TestRoundTripAndMalformedResponse();
  TestRetryIdentityAndReplay();
  TestDeadlineBoundaries();
}

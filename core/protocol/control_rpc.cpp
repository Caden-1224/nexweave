#include "control_rpc.hpp"

#include "../domain/identifiers.hpp"
#include "json_fields.hpp"

namespace nexweave::protocol {
namespace {
using detail::Json;
using domain::ErrorCode;
using domain::OperationResult;
template <class T>
domain::Result<T> rejected(const OperationResult& validation) {
  return domain::Result<T>::failure(validation.error.code, validation.error.message);
}
}  // namespace

OperationResult validate_request(const ControlRequest& request) {
  if (request.version != 1) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "不支持的 RPC 版本");
  }
  if (!domain::is_valid_request_id(request.request_id)) {
    return OperationResult::failure(ErrorCode::kMissingField, "request_id");
  }
  if (request.operation != "start" && request.operation != "query" &&
      request.operation != "cancel" && request.operation != "exit") {
    return OperationResult::failure(ErrorCode::kInvalidInput, "未知 operation");
  }
  if (!request.work_id.empty() && !domain::is_valid_work_id(request.work_id)) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "work_id");
  }
  if (!domain::is_valid_session_id(request.session_id)) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "session_id");
  }
  if (request.deadline.count() <= 0) {
    return OperationResult::failure(ErrorCode::kTimeout, "deadline 必须为正");
  }
  return OperationResult::success();
}

OperationResult validate_response(const ControlResponse& response) {
  if (response.version != 1 || !domain::is_valid_error_code(response.result.error.code)) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "响应版本或错误码无效");
  }
  if (!domain::is_valid_request_id(response.request_id)) {
    return OperationResult::failure(ErrorCode::kMissingField, "request_id");
  }
  return OperationResult::success();
}

domain::Result<std::string> encode_request(const ControlRequest& request) {
  const auto validation = validate_request(request);
  if (!validation.ok()) {
    return rejected<std::string>(validation);
  }
  return detail::dump(Json{{"version", request.version},
                           {"request_id", request.request_id},
                           {"operation", request.operation},
                           {"work_id", request.work_id},
                           {"session_id", request.session_id},
                           {"generation", request.generation},
                           {"deadline_ms", request.deadline.count()}});
}

domain::Result<ControlRequest> decode_request(std::string_view input) {
  return detail::decode<ControlRequest>(
      input,
      {"version", "request_id", "operation", "work_id", "session_id", "generation", "deadline_ms"},
      [](const Json& json) {
        ControlRequest request;
        request.version = detail::integer<std::uint32_t>(json, "version");
        request.request_id = json.at("request_id").get<std::string>();
        request.operation = json.at("operation").get<std::string>();
        request.work_id = json.at("work_id").get<std::string>();
        request.session_id = json.at("session_id").get<std::string>();
        request.generation = detail::integer<std::uint64_t>(json, "generation");
        request.deadline =
            std::chrono::milliseconds(detail::integer<std::int64_t>(json, "deadline_ms"));
        const auto validation = validate_request(request);
        return validation.ok() ? domain::Result<ControlRequest>::success(std::move(request))
                               : rejected<ControlRequest>(validation);
      });
}

domain::Result<std::string> encode_response(const ControlResponse& response) {
  const auto validation = validate_response(response);
  if (!validation.ok()) {
    return rejected<std::string>(validation);
  }
  return detail::dump(Json{{"version", response.version},
                           {"request_id", response.request_id},
                           {"ok", response.result.ok()},
                           {"error_code", static_cast<int>(response.result.error.code)},
                           {"message", response.result.error.message},
                           {"replayed", response.replayed}});
}

domain::Result<ControlResponse> decode_response(std::string_view input) {
  return detail::decode<ControlResponse>(
      input, {"version", "request_id", "ok", "error_code", "message", "replayed"},
      [](const Json& json) {
        ControlResponse response;
        response.version = detail::integer<std::uint32_t>(json, "version");
        response.request_id = json.at("request_id").get<std::string>();
        response.result.error.code =
            static_cast<ErrorCode>(detail::integer<int>(json, "error_code"));
        response.result.error.message = json.at("message").get<std::string>();
        response.replayed = json.at("replayed").get<bool>();
        const auto validation = validate_response(response);
        if (!validation.ok()) {
          return rejected<ControlResponse>(validation);
        }
        if (json.at("ok").get<bool>() != response.result.ok()) {
          return domain::Result<ControlResponse>::failure(ErrorCode::kInvalidInput,
                                                          "响应 ok 与错误码不一致");
        }
        return domain::Result<ControlResponse>::success(std::move(response));
      });
}

OperationResult validate_retry(const ControlRequest& original, const ControlRequest& retry) {
  const auto first = validate_request(original);
  if (!first.ok()) {
    return first;
  }
  const auto repeated = validate_request(retry);
  if (!repeated.ok()) {
    return repeated;
  }
  if (original.version != retry.version || original.request_id != retry.request_id ||
      original.operation != retry.operation || original.work_id != retry.work_id ||
      original.session_id != retry.session_id || original.generation != retry.generation ||
      original.deadline != retry.deadline) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "重复请求的标识或负载冲突");
  }
  return OperationResult::success();
}

OperationResult check_deadline(const ControlRequest& request, std::chrono::milliseconds elapsed) {
  const auto validation = validate_request(request);
  if (!validation.ok()) {
    return validation;
  }
  if (elapsed.count() < 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "耗时不能为负");
  }
  // 直接比较时长而不做起点+预算加法，避免极大预算溢出；重试不能重置首次计时起点。
  if (elapsed >= request.deadline) {
    return OperationResult::failure(ErrorCode::kTimeout, "请求处理预算已耗尽");
  }
  return OperationResult::success();
}

domain::Result<ControlResponse> replay_response(const ControlRequest& original,
                                                const ControlRequest& retry,
                                                const ControlResponse& completed) {
  const auto validation = validate_retry(original, retry);
  if (!validation.ok()) {
    return rejected<ControlResponse>(validation);
  }
  const auto response_validation = validate_response(completed);
  if (!response_validation.ok()) {
    return rejected<ControlResponse>(response_validation);
  }
  if (completed.request_id != original.request_id) {
    return domain::Result<ControlResponse>::failure(ErrorCode::kInvalidInput, "缓存响应归属不一致");
  }
  auto response = completed;
  response.replayed = true;
  return domain::Result<ControlResponse>::success(std::move(response));
}
}  // namespace nexweave::protocol

#include "control_rpc.hpp"
#include <nlohmann/json.hpp>
#include "../domain/identifiers.hpp"
namespace nexweave::protocol {
namespace {
using Json = nlohmann::json;

template <class T>
domain::Result<T> bad(domain::ErrorCode code, std::string message) {
  return domain::Result<T>::failure(code, std::move(message));
}

domain::ErrorCode code(const Json& value, bool& valid) noexcept {
  if (!value.is_number_integer()) {
    valid = false;
    return domain::ErrorCode::kInvalidInput;
  }
  const auto number = value.get<int>();
  if (number < 0 || number > static_cast<int>(domain::ErrorCode::kDeviceFailure)) {
    valid = false;
    return domain::ErrorCode::kInvalidInput;
  }
  valid = true;
  return static_cast<domain::ErrorCode>(number);
}
}
domain::OperationResult validate_request(const ControlRequest&r)noexcept{if(r.version!=1)return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,"不支持的 RPC 版本");if(!domain::is_valid_request_id(r.request_id))return domain::OperationResult::failure(domain::ErrorCode::kMissingField,"request_id");if(r.operation!="start"&&r.operation!="query"&&r.operation!="cancel"&&r.operation!="exit")return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,"未知 operation");if(!r.work_id.empty()&&!domain::is_valid_work_id(r.work_id))return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,"work_id");if(!domain::is_valid_session_id(r.session_id))return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,"session_id");if(r.deadline.count()<=0)return domain::OperationResult::failure(domain::ErrorCode::kTimeout,"deadline 必须为正");return domain::OperationResult::success();}
domain::OperationResult validate_response(const ControlResponse&r)noexcept{if(r.version!=1)return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,"不支持的 RPC 版本");if(!domain::is_valid_request_id(r.request_id))return domain::OperationResult::failure(domain::ErrorCode::kMissingField,"request_id");return domain::OperationResult::success();}
domain::Result<std::string> encode_request(const ControlRequest&r){auto v=validate_request(r);if(!v.ok())return bad<std::string>(v.error.code,v.error.message);Json j={{"version",r.version},{"request_id",r.request_id},{"operation",r.operation},{"work_id",r.work_id},{"session_id",r.session_id},{"generation",r.generation},{"deadline_ms",r.deadline.count()}};return domain::Result<std::string>::success(j.dump());}
domain::Result<ControlRequest> decode_request(std::string_view s){try{auto j=Json::parse(s.begin(),s.end());if(!j.is_object()||j.size()!=7)return bad<ControlRequest>(domain::ErrorCode::kInvalidInput,"请求字段不完整");ControlRequest r;r.version=j.at("version").get<std::uint32_t>();r.request_id=j.at("request_id").get<std::string>();r.operation=j.at("operation").get<std::string>();r.work_id=j.at("work_id").get<std::string>();r.session_id=j.at("session_id").get<std::string>();r.generation=j.at("generation").get<std::uint64_t>();r.deadline=std::chrono::milliseconds(j.at("deadline_ms").get<std::int64_t>());auto v=validate_request(r);if(!v.ok())return bad<ControlRequest>(v.error.code,v.error.message);return domain::Result<ControlRequest>::success(std::move(r));}catch(const std::exception&e){return bad<ControlRequest>(domain::ErrorCode::kInvalidInput,std::string("请求 JSON 无效: ")+e.what());}}
domain::Result<std::string> encode_response(const ControlResponse&r){auto v=validate_response(r);if(!v.ok())return bad<std::string>(v.error.code,v.error.message);Json j={{"version",r.version},{"request_id",r.request_id},{"ok",r.result.ok()},{"error_code",static_cast<int>(r.result.error.code)},{"message",r.result.error.message},{"replayed",r.replayed}};return domain::Result<std::string>::success(j.dump());}
domain::Result<ControlResponse> decode_response(std::string_view s){try{auto j=Json::parse(s.begin(),s.end());if(!j.is_object()||j.size()!=6)return bad<ControlResponse>(domain::ErrorCode::kInvalidInput,"响应字段不完整");ControlResponse r;r.version=j.at("version").get<std::uint32_t>();r.request_id=j.at("request_id").get<std::string>();bool ok=j.at("ok").get<bool>(),valid=false;r.result.error.code=code(j.at("error_code"),valid);r.result.error.message=j.at("message").get<std::string>();r.replayed=j.at("replayed").get<bool>();if(!valid||ok!=r.result.ok())return bad<ControlResponse>(domain::ErrorCode::kInvalidInput,"响应 ok 与错误码不一致");auto v=validate_response(r);if(!v.ok())return bad<ControlResponse>(v.error.code,v.error.message);return domain::Result<ControlResponse>::success(std::move(r));}catch(const std::exception&e){return bad<ControlResponse>(domain::ErrorCode::kInvalidInput,std::string("响应 JSON 无效: ")+e.what());}}
}

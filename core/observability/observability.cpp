#include "observability.hpp"
#include <cmath>
#include <nlohmann/json.hpp>
#include "../domain/identifiers.hpp"
namespace nexweave::observability {
namespace { using Json=nlohmann::json; template<class T> domain::Result<T> bad(domain::ErrorCode c, std::string m){return domain::Result<T>::failure(c, std::move(m));}
bool nonempty(const std::string& s){return !s.empty();}
#define MAN_FIELDS(X) X(run_id) X(git_commit) X(compiler) X(cmake) X(runtime) X(driver) X(model) X(config_hash) X(input_hash) X(device) X(profile) X(command) X(start_time) X(end_time)
}
domain::OperationResult validate_manifest(const RunManifest& m) noexcept { if(m.version!=1)return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,"不支持的 manifest 版本");
#define CHECK(x) if(!nonempty(m.x)) return domain::OperationResult::failure(domain::ErrorCode::kMissingField,#x);
MAN_FIELDS(CHECK)
#undef CHECK
 return domain::OperationResult::success(); }
domain::OperationResult validate_event(const ObservationEvent& e) noexcept { if(e.version!=1)return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,"不支持的事件版本"); if(!domain::is_valid_request_id(e.request_id)||!domain::is_valid_session_id(e.session_id))return domain::OperationResult::failure(domain::ErrorCode::kMissingField,"事件标识"); if(e.name.empty())return domain::OperationResult::failure(domain::ErrorCode::kMissingField,"name"); return domain::OperationResult::success(); }
domain::OperationResult validate_metric(const MetricRecord& m) noexcept { if(m.version!=1)return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,"不支持的指标版本"); if(!domain::is_valid_request_id(m.request_id)||!domain::is_valid_session_id(m.session_id))return domain::OperationResult::failure(domain::ErrorCode::kMissingField,"指标标识"); if(m.name.empty()||m.unit.empty())return domain::OperationResult::failure(domain::ErrorCode::kMissingField,"name/unit"); if(!std::isfinite(m.value))return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,"指标值必须有限"); return domain::OperationResult::success(); }
domain::Result<std::string> encode_manifest(const RunManifest&m){auto v=validate_manifest(m);if(!v.ok())return bad<std::string>(v.error.code, v.error.message);Json j={{"version",m.version}};
#define PUT(x) j[#x]=m.x;
MAN_FIELDS(PUT)
#undef PUT
 return domain::Result<std::string>::success(j.dump());}
domain::Result<RunManifest> decode_manifest(std::string_view s){try{auto j=Json::parse(s.begin(),s.end());if(!j.is_object()||j.size()!=15)return bad<RunManifest>(domain::ErrorCode::kMissingField,"manifest 字段不完整");RunManifest m;m.version=j.at("version").get<std::uint32_t>();
#define GET(x) m.x=j.at(#x).get<std::string>();
MAN_FIELDS(GET)
#undef GET
 auto v=validate_manifest(m);if(!v.ok())return domain::Result<RunManifest>::failure(v.error.code,v.error.message);return domain::Result<RunManifest>::success(std::move(m));}catch(const std::exception&e){return bad<RunManifest>(domain::ErrorCode::kInvalidInput,std::string("manifest JSON 无效: ")+e.what());}}
domain::Result<std::string> encode_event(const ObservationEvent&e){auto v=validate_event(e);if(!v.ok())return bad<std::string>(v.error.code, v.error.message);Json j={{"version",e.version},{"request_id",e.request_id},{"session_id",e.session_id},{"generation",e.generation},{"sequence",e.sequence},{"timestamp_ms",e.timestamp_ms},{"name",e.name},{"attributes",e.attributes}};return domain::Result<std::string>::success(j.dump());}
domain::Result<ObservationEvent> decode_event(std::string_view s){try{auto j=Json::parse(s.begin(),s.end());if(!j.is_object()||j.size()!=8)return bad<ObservationEvent>(domain::ErrorCode::kMissingField,"事件字段不完整");ObservationEvent e;e.version=j.at("version").get<std::uint32_t>();e.request_id=j.at("request_id").get<std::string>();e.session_id=j.at("session_id").get<std::string>();e.generation=j.at("generation").get<std::uint64_t>();e.sequence=j.at("sequence").get<std::uint64_t>();e.timestamp_ms=j.at("timestamp_ms").get<std::uint64_t>();e.name=j.at("name").get<std::string>();e.attributes=j.at("attributes").get<std::map<std::string,std::string>>();auto v=validate_event(e);if(!v.ok())return domain::Result<ObservationEvent>::failure(v.error.code,v.error.message);return domain::Result<ObservationEvent>::success(std::move(e));}catch(const std::exception&e){return bad<ObservationEvent>(domain::ErrorCode::kInvalidInput,std::string("事件 JSON 无效: ")+e.what());}}
domain::Result<std::string> encode_metric(const MetricRecord&m){auto v=validate_metric(m);if(!v.ok())return bad<std::string>(v.error.code, v.error.message);Json j={{"version",m.version},{"request_id",m.request_id},{"session_id",m.session_id},{"generation",m.generation},{"timestamp_ms",m.timestamp_ms},{"name",m.name},{"unit",m.unit},{"value",m.value}};return domain::Result<std::string>::success(j.dump());}
domain::Result<MetricRecord> decode_metric(std::string_view s){try{auto j=Json::parse(s.begin(),s.end());if(!j.is_object()||j.size()!=8)return bad<MetricRecord>(domain::ErrorCode::kMissingField,"指标字段不完整");MetricRecord m;m.version=j.at("version").get<std::uint32_t>();m.request_id=j.at("request_id").get<std::string>();m.session_id=j.at("session_id").get<std::string>();m.generation=j.at("generation").get<std::uint64_t>();m.timestamp_ms=j.at("timestamp_ms").get<std::uint64_t>();m.name=j.at("name").get<std::string>();m.unit=j.at("unit").get<std::string>();m.value=j.at("value").get<double>();auto v=validate_metric(m);if(!v.ok())return domain::Result<MetricRecord>::failure(v.error.code,v.error.message);return domain::Result<MetricRecord>::success(std::move(m));}catch(const std::exception&e){return bad<MetricRecord>(domain::ErrorCode::kInvalidInput,std::string("指标 JSON 无效: ")+e.what());}}
}


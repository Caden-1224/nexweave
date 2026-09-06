#include "observability.hpp"

#include <cmath>

#include <nlohmann/json.hpp>

#include "../domain/identifiers.hpp"

namespace nexweave::observability {
namespace {

using Json = nlohmann::json;

template <class T>
domain::Result<T> bad(domain::ErrorCode code, std::string message) {
  return domain::Result<T>::failure(code, std::move(message));
}

bool nonempty(const std::string& value) { return !value.empty(); }

#define MAN_FIELDS(X) \
  X(run_id) X(git_commit) X(compiler) X(cmake) X(runtime) X(driver) \
      X(model) X(config_hash) X(input_hash) X(device) X(profile) X(command) \
          X(start_time) X(end_time)

}  // namespace

domain::OperationResult validate_manifest(const RunManifest& manifest) noexcept {
  if (manifest.version != 1) {
    return domain::OperationResult::failure(
        domain::ErrorCode::kInvalidInput, "不支持的 manifest 版本");
  }
#define CHECK(field) \
  if (!nonempty(manifest.field)) { \
    return domain::OperationResult::failure(domain::ErrorCode::kMissingField, #field); \
  }
  MAN_FIELDS(CHECK)
#undef CHECK
  return domain::OperationResult::success();
}

domain::OperationResult validate_event(const ObservationEvent& event) noexcept {
  if (event.version != 1) {
    return domain::OperationResult::failure(
        domain::ErrorCode::kInvalidInput, "不支持的事件版本");
  }
  if (!domain::is_valid_request_id(event.request_id) ||
      !domain::is_valid_session_id(event.session_id)) {
    return domain::OperationResult::failure(domain::ErrorCode::kMissingField,
                                            "事件标识");
  }
  if (event.name.empty()) {
    return domain::OperationResult::failure(domain::ErrorCode::kMissingField,
                                            "name");
  }
  return domain::OperationResult::success();
}

domain::OperationResult validate_metric(const MetricRecord& metric) noexcept {
  if (metric.version != 1) {
    return domain::OperationResult::failure(
        domain::ErrorCode::kInvalidInput, "不支持的指标版本");
  }
  if (!domain::is_valid_request_id(metric.request_id) ||
      !domain::is_valid_session_id(metric.session_id)) {
    return domain::OperationResult::failure(domain::ErrorCode::kMissingField,
                                            "指标标识");
  }
  if (metric.name.empty() || metric.unit.empty()) {
    return domain::OperationResult::failure(domain::ErrorCode::kMissingField,
                                            "name/unit");
  }
  if (!std::isfinite(metric.value)) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "指标值必须有限");
  }
  return domain::OperationResult::success();
}

domain::Result<std::string> encode_manifest(const RunManifest& manifest) {
  const auto validation = validate_manifest(manifest);
  if (!validation.ok()) {
    return bad<std::string>(validation.error.code, validation.error.message);
  }
  Json json = {{"version", manifest.version}};
#define PUT(field) json[#field] = manifest.field;
  MAN_FIELDS(PUT)
#undef PUT
  return domain::Result<std::string>::success(json.dump());
}

domain::Result<RunManifest> decode_manifest(std::string_view serialized) {
  try {
    const auto json = Json::parse(serialized.begin(), serialized.end());
    if (!json.is_object() || json.size() != 15) {
      return bad<RunManifest>(domain::ErrorCode::kMissingField,
                              "manifest 字段不完整");
    }
    RunManifest manifest;
    manifest.version = json.at("version").get<std::uint32_t>();
#define GET(field) manifest.field = json.at(#field).get<std::string>();
    MAN_FIELDS(GET)
#undef GET
    const auto validation = validate_manifest(manifest);
    if (!validation.ok()) {
      return domain::Result<RunManifest>::failure(validation.error.code,
                                                  validation.error.message);
    }
    return domain::Result<RunManifest>::success(std::move(manifest));
  } catch (const std::exception& error) {
    return bad<RunManifest>(domain::ErrorCode::kInvalidInput,
                            std::string("manifest JSON 无效: ") + error.what());
  }
}

domain::Result<std::string> encode_event(const ObservationEvent& event) {
  const auto validation = validate_event(event);
  if (!validation.ok()) {
    return bad<std::string>(validation.error.code, validation.error.message);
  }
  Json json = {{"version", event.version},
               {"request_id", event.request_id},
               {"session_id", event.session_id},
               {"generation", event.generation},
               {"sequence", event.sequence},
               {"timestamp_ms", event.timestamp_ms},
               {"name", event.name},
               {"attributes", event.attributes}};
  return domain::Result<std::string>::success(json.dump());
}

domain::Result<ObservationEvent> decode_event(std::string_view serialized) {
  try {
    const auto json = Json::parse(serialized.begin(), serialized.end());
    if (!json.is_object() || json.size() != 8) {
      return bad<ObservationEvent>(domain::ErrorCode::kMissingField,
                                   "事件字段不完整");
    }
    ObservationEvent event;
    event.version = json.at("version").get<std::uint32_t>();
    event.request_id = json.at("request_id").get<std::string>();
    event.session_id = json.at("session_id").get<std::string>();
    event.generation = json.at("generation").get<std::uint64_t>();
    event.sequence = json.at("sequence").get<std::uint64_t>();
    event.timestamp_ms = json.at("timestamp_ms").get<std::uint64_t>();
    event.name = json.at("name").get<std::string>();
    event.attributes =
        json.at("attributes").get<std::map<std::string, std::string>>();
    const auto validation = validate_event(event);
    if (!validation.ok()) {
      return domain::Result<ObservationEvent>::failure(
          validation.error.code, validation.error.message);
    }
    return domain::Result<ObservationEvent>::success(std::move(event));
  } catch (const std::exception& error) {
    return bad<ObservationEvent>(domain::ErrorCode::kInvalidInput,
                                 std::string("事件 JSON 无效: ") + error.what());
  }
}

domain::Result<std::string> encode_metric(const MetricRecord& metric) {
  const auto validation = validate_metric(metric);
  if (!validation.ok()) {
    return bad<std::string>(validation.error.code, validation.error.message);
  }
  Json json = {{"version", metric.version},
               {"request_id", metric.request_id},
               {"session_id", metric.session_id},
               {"generation", metric.generation},
               {"timestamp_ms", metric.timestamp_ms},
               {"name", metric.name},
               {"unit", metric.unit},
               {"value", metric.value}};
  return domain::Result<std::string>::success(json.dump());
}

domain::Result<MetricRecord> decode_metric(std::string_view serialized) {
  try {
    const auto json = Json::parse(serialized.begin(), serialized.end());
    if (!json.is_object() || json.size() != 8) {
      return bad<MetricRecord>(domain::ErrorCode::kMissingField,
                              "指标字段不完整");
    }
    MetricRecord metric;
    metric.version = json.at("version").get<std::uint32_t>();
    metric.request_id = json.at("request_id").get<std::string>();
    metric.session_id = json.at("session_id").get<std::string>();
    metric.generation = json.at("generation").get<std::uint64_t>();
    metric.timestamp_ms = json.at("timestamp_ms").get<std::uint64_t>();
    metric.name = json.at("name").get<std::string>();
    metric.unit = json.at("unit").get<std::string>();
    metric.value = json.at("value").get<double>();
    const auto validation = validate_metric(metric);
    if (!validation.ok()) {
      return domain::Result<MetricRecord>::failure(validation.error.code,
                                                   validation.error.message);
    }
    return domain::Result<MetricRecord>::success(std::move(metric));
  } catch (const std::exception& error) {
    return bad<MetricRecord>(domain::ErrorCode::kInvalidInput,
                             std::string("指标 JSON 无效: ") + error.what());
  }
}

}  // namespace nexweave::observability

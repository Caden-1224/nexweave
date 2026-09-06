#include "observability.hpp"

#include <cmath>

#include "../domain/identifiers.hpp"
#include "../protocol/json_fields.hpp"

namespace nexweave::observability {
namespace {
namespace fields = protocol::detail;
using domain::ErrorCode;
using domain::OperationResult;
using fields::Json;
template <class T>
domain::Result<T> rejected(const OperationResult& validation) {
  return domain::Result<T>::failure(validation.error.code, validation.error.message);
}
}  // namespace

OperationResult validate_manifest(const RunManifest& manifest) {
  if (manifest.version != 1) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "不支持的 manifest 版本");
  }
  if (manifest.run_id.empty() || manifest.git_commit.empty() || manifest.compiler.empty() ||
      manifest.cmake.empty() || manifest.runtime.empty() || manifest.driver.empty() ||
      manifest.model.empty() || manifest.config_hash.empty() || manifest.input_hash.empty() ||
      manifest.device.empty() || manifest.profile.empty() || manifest.command.empty() ||
      manifest.start_time.empty() || manifest.end_time.empty()) {
    return OperationResult::failure(ErrorCode::kMissingField, "运行清单必填字段为空");
  }
  return OperationResult::success();
}

OperationResult validate_event(const ObservationEvent& event) {
  if (event.version != 1) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "不支持的事件版本");
  }
  if (!domain::is_valid_request_id(event.request_id) ||
      !domain::is_valid_session_id(event.session_id) || event.name.empty()) {
    return OperationResult::failure(ErrorCode::kMissingField, "事件标识或名称无效");
  }
  return OperationResult::success();
}

OperationResult validate_metric(const MetricRecord& metric) {
  if (metric.version != 1) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "不支持的指标版本");
  }
  if (!domain::is_valid_request_id(metric.request_id) ||
      !domain::is_valid_session_id(metric.session_id) || metric.name.empty() ||
      metric.unit.empty()) {
    return OperationResult::failure(ErrorCode::kMissingField, "指标标识、名称或单位无效");
  }
  if (!std::isfinite(metric.value)) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "指标值必须有限");
  }
  return OperationResult::success();
}

domain::Result<std::string> encode_manifest(const RunManifest& record) {
  const auto validation = validate_manifest(record);
  if (!validation.ok()) {
    return rejected<std::string>(validation);
  }
  return fields::dump(Json{{"version", record.version},
                           {"run_id", record.run_id},
                           {"git_commit", record.git_commit},
                           {"compiler", record.compiler},
                           {"cmake", record.cmake},
                           {"runtime", record.runtime},
                           {"driver", record.driver},
                           {"model", record.model},
                           {"config_hash", record.config_hash},
                           {"input_hash", record.input_hash},
                           {"device", record.device},
                           {"profile", record.profile},
                           {"command", record.command},
                           {"start_time", record.start_time},
                           {"end_time", record.end_time}});
}

domain::Result<RunManifest> decode_manifest(std::string_view input) {
  return fields::decode<RunManifest>(
      input,
      {"version", "run_id", "git_commit", "compiler", "cmake", "runtime", "driver", "model",
       "config_hash", "input_hash", "device", "profile", "command", "start_time", "end_time"},
      [](const Json& json) {
        RunManifest record;
        record.version = fields::integer<std::uint32_t>(json, "version");
        record.run_id = json.at("run_id").get<std::string>();
        record.git_commit = json.at("git_commit").get<std::string>();
        record.compiler = json.at("compiler").get<std::string>();
        record.cmake = json.at("cmake").get<std::string>();
        record.runtime = json.at("runtime").get<std::string>();
        record.driver = json.at("driver").get<std::string>();
        record.model = json.at("model").get<std::string>();
        record.config_hash = json.at("config_hash").get<std::string>();
        record.input_hash = json.at("input_hash").get<std::string>();
        record.device = json.at("device").get<std::string>();
        record.profile = json.at("profile").get<std::string>();
        record.command = json.at("command").get<std::string>();
        record.start_time = json.at("start_time").get<std::string>();
        record.end_time = json.at("end_time").get<std::string>();
        const auto validation = validate_manifest(record);
        return validation.ok() ? domain::Result<RunManifest>::success(std::move(record))
                               : rejected<RunManifest>(validation);
      });
}

domain::Result<std::string> encode_event(const ObservationEvent& record) {
  const auto validation = validate_event(record);
  if (!validation.ok()) {
    return rejected<std::string>(validation);
  }
  return fields::dump(Json{{"version", record.version},
                           {"request_id", record.request_id},
                           {"session_id", record.session_id},
                           {"generation", record.generation},
                           {"sequence", record.sequence},
                           {"timestamp_ms", record.timestamp_ms},
                           {"name", record.name},
                           {"attributes", record.attributes}});
}

domain::Result<ObservationEvent> decode_event(std::string_view input) {
  return fields::decode<ObservationEvent>(
      input,
      {"version", "request_id", "session_id", "generation", "sequence", "timestamp_ms", "name",
       "attributes"},
      [](const Json& json) {
        ObservationEvent record;
        record.version = fields::integer<std::uint32_t>(json, "version");
        record.request_id = json.at("request_id").get<std::string>();
        record.session_id = json.at("session_id").get<std::string>();
        record.generation = fields::integer<std::uint64_t>(json, "generation");
        record.sequence = fields::integer<std::uint64_t>(json, "sequence");
        record.timestamp_ms = fields::integer<std::uint64_t>(json, "timestamp_ms");
        record.name = json.at("name").get<std::string>();
        record.attributes = json.at("attributes").get<std::map<std::string, std::string>>();
        const auto validation = validate_event(record);
        return validation.ok() ? domain::Result<ObservationEvent>::success(std::move(record))
                               : rejected<ObservationEvent>(validation);
      });
}

domain::Result<std::string> encode_metric(const MetricRecord& record) {
  const auto validation = validate_metric(record);
  if (!validation.ok()) {
    return rejected<std::string>(validation);
  }
  return fields::dump(Json{{"version", record.version},
                           {"request_id", record.request_id},
                           {"session_id", record.session_id},
                           {"generation", record.generation},
                           {"timestamp_ms", record.timestamp_ms},
                           {"name", record.name},
                           {"unit", record.unit},
                           {"value", record.value}});
}

domain::Result<MetricRecord> decode_metric(std::string_view input) {
  return fields::decode<MetricRecord>(
      input,
      {"version", "request_id", "session_id", "generation", "timestamp_ms", "name", "unit",
       "value"},
      [](const Json& json) {
        MetricRecord record;
        record.version = fields::integer<std::uint32_t>(json, "version");
        record.request_id = json.at("request_id").get<std::string>();
        record.session_id = json.at("session_id").get<std::string>();
        record.generation = fields::integer<std::uint64_t>(json, "generation");
        record.timestamp_ms = fields::integer<std::uint64_t>(json, "timestamp_ms");
        record.name = json.at("name").get<std::string>();
        record.unit = json.at("unit").get<std::string>();
        if (!json.at("value").is_number()) {
          throw std::invalid_argument("指标值必须是 JSON 数值");
        }
        record.value = json.at("value").get<double>();
        const auto validation = validate_metric(record);
        return validation.ok() ? domain::Result<MetricRecord>::success(std::move(record))
                               : rejected<MetricRecord>(validation);
      });
}
}  // namespace nexweave::observability

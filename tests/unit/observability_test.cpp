// 验证三类记录成功往返与缺字段/非法数值失败；CHECK 在 Release/Debug 都执行。
// 固定时间与字段是合成夹具，不是性能实验，不创建记录文件或访问设备。
#include "../../core/observability/observability.hpp"

#include <limits>
#include <nlohmann/json.hpp>

#include "../test_support.hpp"

namespace {
using namespace nexweave::observability;
RunManifest Manifest() {
  RunManifest manifest;
  manifest.run_id = "r1";
  manifest.git_commit = "abc";
  manifest.compiler = "g++";
  manifest.cmake = "3.22";
  manifest.runtime = "linux";
  manifest.driver = "none";
  manifest.model = "fake";
  manifest.config_hash = "c";
  manifest.input_hash = "i";
  manifest.device = "wsl";
  manifest.profile = "mock";
  manifest.command = "run";
  manifest.start_time = "2026-01-01T00:00:00Z";
  manifest.end_time = "2026-01-01T00:00:01Z";
  return manifest;
}
void TestManifest() {
  auto manifest = Manifest();
  const auto encoded = encode_manifest(manifest);
  CHECK(encoded.ok());
  const auto decoded = decode_manifest(*encoded.value);
  CHECK(decoded.ok());
  CHECK(*encode_manifest(*decoded.value).value == *encoded.value);
  manifest.model.clear();
  CHECK(!validate_manifest(manifest).ok());
  CHECK(!encode_manifest(manifest).ok());
  // 版本兼容边界：布尔、小数、回绕和未知版本不能伪装成 v1。
  for (const auto& version : {nlohmann::json(true), nlohmann::json(1.5),
                              nlohmann::json(4294967297ULL), nlohmann::json(2)}) {
    auto json = nlohmann::json::parse(*encoded.value);
    json["version"] = version;
    CHECK(!decode_manifest(json.dump()).ok());
  }
  auto extra = nlohmann::json::parse(*encoded.value);
  extra["extra"] = "field";
  CHECK(!decode_manifest(extra.dump()).ok());
}
void TestEvent() {
  ObservationEvent event;
  event.request_id = "req-1";
  event.session_id = "sess-1";
  event.name = "done";
  event.attributes["status"] = "ok";
  event.generation = 3;
  event.sequence = 4;
  event.timestamp_ms = 5;
  const auto encoded = encode_event(event);
  CHECK(encoded.ok());
  const auto decoded = decode_event(*encoded.value);
  CHECK(decoded.ok());
  CHECK(*encode_event(*decoded.value).value == *encoded.value);
  event.name.clear();
  CHECK(!validate_event(event).ok());
  CHECK(!decode_event("{}").ok());
  CHECK(!decode_event("not json").ok());
}
void TestMetric() {
  MetricRecord metric;
  metric.request_id = "req-1";
  metric.session_id = "sess-1";
  metric.name = "latency";
  metric.unit = "ms";
  metric.value = 12.5;
  const auto encoded = encode_metric(metric);
  CHECK(encoded.ok());
  const auto decoded = decode_metric(*encoded.value);
  CHECK(decoded.ok());
  CHECK(decoded.value->value == 12.5);
  CHECK(*encode_metric(*decoded.value).value == *encoded.value);
  // NaN/Inf 不能落盘成可比较指标，必须在序列化前返回失败。
  for (double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    metric.value = bad;
    CHECK(!validate_metric(metric).ok());
    CHECK(!encode_metric(metric).ok());
  }
}
}  // namespace

int main() {
  TestManifest();
  TestEvent();
  TestMetric();
}

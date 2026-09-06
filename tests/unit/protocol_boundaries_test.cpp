// 协议边界回归：使用真实公共编解码入口，拒绝能被 JSON 隐式转换伪装成合法值的输入。
// 夹具为纯内存 v1 消息；不依赖时钟、网络、线程和设备。任何配置失败均非零退出。
#include <limits>
#include <nlohmann/json.hpp>

#include "../../core/observability/observability.hpp"
#include "../../core/protocol/control_rpc.hpp"
#include "../../core/protocol/data_event.hpp"
#include "../test_support.hpp"

namespace {
using Json = nlohmann::json;
namespace p = nexweave::protocol;
namespace o = nexweave::observability;
namespace d = nexweave::domain;

// 保证坏输入返回结构化失败且无值；同时覆盖缺字段和未知字段的 v1 闭集策略。
template <class Decode>
void CheckNumericFields(const Json& good, std::initializer_list<const char*> fields,
                        Decode decode) {
  CHECK(decode(good.dump()).ok());
  for (const auto* field : fields) {
    for (const auto& bad :
         {Json(-1), Json(1.5), Json(true), Json(false), Json("1"), Json(nullptr), Json(1e30)}) {
      auto input = good;
      input[field] = bad;
      const auto result = decode(input.dump());
      CHECK(!result.ok());
      CHECK(!result.value.has_value());
    }
    auto missing = good;
    missing.erase(field);
    CHECK(!decode(missing.dump()).ok());
  }
  auto extra = good;
  extra["unknown"] = 0;
  CHECK(!decode(extra.dump()).ok());
  auto wrapped = good;
  wrapped["version"] = 4294967297ULL;
  CHECK(!decode(wrapped.dump()).ok());
}

void TestControlNumbers() {
  p::ControlRequest request;
  request.request_id = "req-1";
  request.operation = "start";
  request.deadline = std::chrono::milliseconds(100);
  const auto good = Json::parse(*p::encode_request(request).value);
  CheckNumericFields(good, {"version", "generation", "deadline_ms"}, p::decode_request);
  auto maximum = good;
  maximum["generation"] = std::numeric_limits<std::uint64_t>::max();
  maximum["deadline_ms"] = std::numeric_limits<std::int64_t>::max();
  CHECK(p::decode_request(maximum.dump()).ok());
  maximum["deadline_ms"] = std::numeric_limits<std::uint64_t>::max();
  CHECK(!p::decode_request(maximum.dump()).ok());

  p::ControlResponse response;
  response.request_id = "req-1";
  const auto good_response = Json::parse(*p::encode_response(response).value);
  CheckNumericFields(good_response, {"version", "error_code"}, p::decode_response);
  auto wrapped_error = good_response;
  wrapped_error["error_code"] = 4294967296ULL;
  CHECK(!p::decode_response(wrapped_error.dump()).ok());
  response.result.error.code = static_cast<d::ErrorCode>(99);
  CHECK(!p::encode_response(response).ok());
}

void TestDataNumbersAndMetadata() {
  p::DataEvent event;
  event.request_id = "req-1";
  event.type = p::DataEventType::kPartial;
  const auto good = Json::parse(*p::encode_event_metadata(event).value);
  CheckNumericFields(
      good, {"version", "generation", "sequence", "frame_index", "error_code", "pcm_bytes"},
      p::decode_event_metadata);
  auto frame_overflow = good;
  frame_overflow["frame_index"] = 4294967296ULL;
  CHECK(!p::decode_event_metadata(frame_overflow.dump()).ok());

  // PCM 允许尚无二进制载荷，但头字段必须在解码时校验，不能等 attach 才发现归属错误。
  event.type = p::DataEventType::kPcm;
  event.pcm.assign(640, 0);
  const auto pcm = Json::parse(*p::encode_event_metadata(event).value);
  for (const auto& patch :
       {Json{{"version", 2}}, Json{{"request_id", ""}}, Json{{"session_id", "/"}},
        Json{{"error_code", 1}}, Json{{"pcm_bytes", 639}}}) {
    auto bad = pcm;
    bad.update(patch);
    CHECK(!p::decode_event_metadata(bad.dump()).ok());
  }
  event.pcm.clear();
  event.type = static_cast<p::DataEventType>(255);
  CHECK(!p::validate_event(event).ok());
  CHECK(!p::encode_event_metadata(event).ok());
  event.type = p::DataEventType::kError;
  event.error_code = static_cast<d::ErrorCode>(99);
  CHECK(!p::encode_event_metadata(event).ok());
}

void TestObservationNumbers() {
  o::ObservationEvent event;
  event.request_id = "req-1";
  event.name = "done";
  CheckNumericFields(Json::parse(*o::encode_event(event).value),
                     {"version", "generation", "sequence", "timestamp_ms"}, o::decode_event);
  o::MetricRecord metric;
  metric.request_id = "req-1";
  metric.name = "latency";
  metric.unit = "ms";
  CheckNumericFields(Json::parse(*o::encode_metric(metric).value),
                     {"version", "generation", "timestamp_ms"}, o::decode_metric);
  // 合法指标可以有小数，但布尔值不得伪装成数值。
  auto bad = Json::parse(*o::encode_metric(metric).value);
  bad["value"] = true;
  CHECK(!o::decode_metric(bad.dump()).ok());
}

void TestTextEncodingFailure() {
  // 外部诊断字节不是合法 UTF-8 时，编码返回失败而不泄漏 JSON 库异常。
  p::ControlResponse response;
  response.request_id = "req-1";
  response.result.error.message.assign(1, static_cast<char>(0xff));
  CHECK(!p::encode_response(response).ok());
}
}  // namespace

int main() {
  TestControlNumbers();
  TestDataNumbersAndMetadata();
  TestObservationNumbers();
  TestTextEncodingFailure();
}

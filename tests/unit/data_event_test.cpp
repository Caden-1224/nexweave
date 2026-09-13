// 数据面事件契约的单元夹具：验收“元数据与二进制载荷分开传递”这条约定的边界。
//
// 保护的不变量（每条在断言旁注明）：
//   1. 往返完整：合法事件编码后再解码，字段逐个保持一致——只断言“解码成功”不能说明任何
//      字段被正确保留，因此这里逐字段比较。
//   2. 载荷分离：PCM 的元数据里不携带字节，载荷必须经 attach_pcm_payload 显式附加；
//      附加后的字节与原始载荷逐字节一致，长度恒为统一音频契约的一帧。
//   3. 长度校验先于接受：PCM 事件的载荷长度不是 640 时拒绝，而不是截断或补零。
//   4. 终态一致性：done 必须标记 end，error 必须携带非成功错误码，反之亦然。
#include "../test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "../../core/protocol/data_event.hpp"

using nexweave::domain::ErrorCode;
using nexweave::protocol::attach_pcm_payload;
using nexweave::protocol::DataEvent;
using nexweave::protocol::DataEventType;
using nexweave::protocol::decode_event_metadata;
using nexweave::protocol::encode_event_metadata;
using nexweave::protocol::encode_pcm_payload;
using nexweave::protocol::validate_event;

namespace {

// 一帧 PCM 的固定字节数由统一音频契约决定（320 样本 × 2 字节）。
constexpr std::size_t kFrameBytes = 640;

DataEvent PartialEvent() {
  DataEvent event;
  event.request_id = "req-1";
  event.session_id = "sess-1";
  event.generation = 7;
  event.sequence = 3;
  event.type = DataEventType::kPartial;
  event.text = "hi";
  event.frame_index = 2;
  return event;
}

void TestMetadataRoundTripPreservesEveryField() {
  const DataEvent original = PartialEvent();
  const auto encoded = encode_event_metadata(original);
  CHECK(encoded.ok());

  const auto decoded = decode_event_metadata(*encoded.value);
  CHECK(decoded.ok());
  // 逐字段比较：任何一处丢失或错位都必须被看见，而不是被“解码成功”掩盖。
  CHECK(decoded.value->version == original.version);
  CHECK(decoded.value->request_id == original.request_id);
  CHECK(decoded.value->session_id == original.session_id);
  CHECK(decoded.value->generation == original.generation);
  CHECK(decoded.value->sequence == original.sequence);
  CHECK(decoded.value->type == original.type);
  CHECK(decoded.value->text == original.text);
  CHECK(decoded.value->frame_index == original.frame_index);
  CHECK(decoded.value->end == original.end);
  CHECK(decoded.value->error_code == original.error_code);
  CHECK(decoded.value->pcm.empty());
}

void TestErrorEventRequiresMatchingErrorCode() {
  DataEvent event = PartialEvent();
  event.type = DataEventType::kError;
  // error 类型不带错误码：类型与错误码必须一致，否则调用方无法区分“失败”与“正常事件”。
  CHECK(!validate_event(event).ok());

  event.error_code = ErrorCode::kTimeout;
  CHECK(validate_event(event).ok());
  const auto encoded = encode_event_metadata(event);
  CHECK(encoded.ok());
  const auto decoded = decode_event_metadata(*encoded.value);
  CHECK(decoded.ok());
  CHECK(decoded.value->error_code == ErrorCode::kTimeout);
}

void TestDoneMustBeMarkedAsEnd() {
  DataEvent event = PartialEvent();
  event.type = DataEventType::kDone;
  // 未标 end 的 done 会把“局部完成”当成“全部结束”，因此必须拒绝。
  CHECK(!validate_event(event).ok());
  event.end = true;
  CHECK(validate_event(event).ok());
}

void TestPcmMetadataCarriesNoPayloadAndDeclaresFrameLength() {
  DataEvent event = PartialEvent();
  event.type = DataEventType::kPcm;
  event.pcm.assign(kFrameBytes, 7);

  // 载荷不属于元数据：解码出来的元数据里没有字节，长度只以声明值出现。
  const auto encoded = encode_event_metadata(event);
  CHECK(encoded.ok());
  const auto decoded = decode_event_metadata(*encoded.value);
  CHECK(decoded.ok());
  CHECK(decoded.value->pcm.empty());
  CHECK(decoded.value->expected_pcm_bytes == kFrameBytes);

  const auto payload = encode_pcm_payload(event);
  CHECK(payload.ok());

  // 声明长度与载荷一致时才接受，且附加上去的字节与原始载荷逐字节相同。
  const auto attached = attach_pcm_payload(*decoded.value, *payload.value);
  CHECK(attached.ok());
  CHECK(attached.value->pcm == event.pcm);
  CHECK(attached.value->pcm.size() == kFrameBytes);
  CHECK(validate_event(*attached.value).ok());
}

void TestPcmPayloadLengthIsRejectedNotPadded() {
  DataEvent event = PartialEvent();
  event.type = DataEventType::kPcm;
  event.pcm.assign(kFrameBytes - 1, 7);
  // 短一帧的载荷必须被拒绝：截断或补零都会让下游拿到与声明不符的音频。
  CHECK(!validate_event(event).ok());

  const std::vector<std::uint8_t> short_payload(kFrameBytes - 1, 7);
  const auto attached = attach_pcm_payload(event, short_payload);
  CHECK(!attached.ok());
  CHECK(attached.error.code == ErrorCode::kInvalidInput);
}

}  // namespace

int main() {
  try {
    TestMetadataRoundTripPreservesEveryField();
    TestErrorEventRequiresMatchingErrorCode();
    TestDoneMustBeMarkedAsEnd();
    TestPcmMetadataCarriesNoPayloadAndDeclaresFrameLength();
    TestPcmPayloadLengthIsRejectedNotPadded();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "数据面事件用例未通过: %s\n", error.what());
    return 1;
  }
  return 0;
}

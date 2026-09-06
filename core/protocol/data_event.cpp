#include "data_event.hpp"

#include "../domain/identifiers.hpp"
#include "json_fields.hpp"

namespace nexweave::protocol {
namespace {
using detail::Json;
using domain::ErrorCode;
using domain::OperationResult;
constexpr const char* names[] = {"partial", "final", "token", "pcm", "done", "error"};

const char* name(DataEventType type) noexcept {
  const auto index = static_cast<unsigned>(type);
  return index < 6 ? names[index] : "";
}

DataEventType parse_type(const std::string& text) {
  for (unsigned index = 0; index < 6; ++index) {
    if (text == names[index]) {
      return static_cast<DataEventType>(index);
    }
  }
  throw std::invalid_argument("未知事件类型");
}

// 元数据与完整帧共用头校验；不能因为二进制尚未到达而跳过版本/归属验证。
// 此函数没有流历史，不能证明 sequence 单调或 generation 当前有效。
OperationResult validate_header(const DataEvent& event) {
  if (event.version != 1 || name(event.type)[0] == '\0' ||
      !domain::is_valid_error_code(event.error_code)) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "数据面版本、类型或错误码无效");
  }
  if (!domain::is_valid_request_id(event.request_id)) {
    return OperationResult::failure(ErrorCode::kMissingField, "request_id");
  }
  if (!domain::is_valid_session_id(event.session_id)) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "session_id");
  }
  if ((event.type == DataEventType::kError) != (event.error_code != ErrorCode::kNone)) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "错误码与事件类型不一致");
  }
  if (event.type == DataEventType::kDone && !event.end) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "done 必须标记 end");
  }
  return OperationResult::success();
}

template <class T>
domain::Result<T> rejected(const OperationResult& validation) {
  return domain::Result<T>::failure(validation.error.code, validation.error.message);
}
}  // namespace

OperationResult validate_event(const DataEvent& event) {
  const auto header = validate_header(event);
  if (!header.ok()) {
    return header;
  }
  const auto required_bytes = event.type == DataEventType::kPcm ? domain::kAudioFrameBytes : 0;
  if (event.pcm.size() != required_bytes ||
      (event.expected_pcm_bytes != 0 && event.expected_pcm_bytes != required_bytes)) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "事件 PCM 长度不符合声明和帧合同");
  }
  return OperationResult::success();
}

domain::Result<std::string> encode_event_metadata(const DataEvent& event) {
  const auto validation = validate_event(event);
  if (!validation.ok()) {
    return rejected<std::string>(validation);
  }
  return detail::dump(Json{{"version", event.version},
                           {"request_id", event.request_id},
                           {"session_id", event.session_id},
                           {"generation", event.generation},
                           {"sequence", event.sequence},
                           {"type", name(event.type)},
                           {"text", event.text},
                           {"frame_index", event.frame_index},
                           {"end", event.end},
                           {"error_code", static_cast<int>(event.error_code)},
                           {"message", event.message},
                           {"pcm_bytes", event.pcm.size()}});
}

domain::Result<DataEvent> decode_event_metadata(std::string_view input) {
  return detail::decode<DataEvent>(
      input,
      {"version", "request_id", "session_id", "generation", "sequence", "type", "text",
       "frame_index", "end", "error_code", "message", "pcm_bytes"},
      [](const Json& json) {
        DataEvent event;
        event.version = detail::integer<std::uint32_t>(json, "version");
        event.request_id = json.at("request_id").get<std::string>();
        event.session_id = json.at("session_id").get<std::string>();
        event.generation = detail::integer<std::uint64_t>(json, "generation");
        event.sequence = detail::integer<std::uint64_t>(json, "sequence");
        event.type = parse_type(json.at("type").get<std::string>());
        event.text = json.at("text").get<std::string>();
        event.frame_index = detail::integer<std::uint32_t>(json, "frame_index");
        event.end = json.at("end").get<bool>();
        event.error_code = static_cast<ErrorCode>(detail::integer<int>(json, "error_code"));
        event.message = json.at("message").get<std::string>();
        event.expected_pcm_bytes = detail::integer<std::size_t>(json, "pcm_bytes");
        const auto header = validate_header(event);
        if (!header.ok()) {
          return rejected<DataEvent>(header);
        }
        const auto required = event.type == DataEventType::kPcm ? domain::kAudioFrameBytes : 0;
        if (event.expected_pcm_bytes != required) {
          return domain::Result<DataEvent>::failure(ErrorCode::kInvalidInput, "PCM 元数据长度错误");
        }
        return domain::Result<DataEvent>::success(std::move(event));
      });
}

domain::Result<std::vector<std::uint8_t>> encode_pcm_payload(const DataEvent& event) {
  if (event.type != DataEventType::kPcm) {
    return domain::Result<std::vector<std::uint8_t>>::failure(ErrorCode::kInvalidInput,
                                                              "只有 PCM 事件可编码载荷");
  }
  const auto validation = validate_event(event);
  if (!validation.ok()) {
    return rejected<std::vector<std::uint8_t>>(validation);
  }
  return domain::Result<std::vector<std::uint8_t>>::success(event.pcm);
}

domain::Result<DataEvent> attach_pcm_payload(DataEvent event,
                                             const std::vector<std::uint8_t>& payload) {
  const auto header = validate_header(event);
  if (!header.ok()) {
    return rejected<DataEvent>(header);
  }
  if (event.type != DataEventType::kPcm || payload.size() != domain::kAudioFrameBytes ||
      (event.expected_pcm_bytes != 0 && event.expected_pcm_bytes != payload.size())) {
    return domain::Result<DataEvent>::failure(ErrorCode::kInvalidInput, "PCM 头或载荷长度无效");
  }
  // 先验证长度再复制，避免不合法的大载荷造成不必要分配；源头和源载荷始终不变。
  event.pcm = payload;
  return domain::Result<DataEvent>::success(std::move(event));
}
}  // namespace nexweave::protocol

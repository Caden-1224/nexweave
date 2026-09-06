#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "../domain/audio_frame.hpp"
#include "../domain/error.hpp"
namespace nexweave::protocol {
// 数据面事件值对象：传递文本增量、PCM 帧和终态通知；不拥有外部线程、socket 或文件。
// 输入前提是 request_id/session_id 已按领域标识规则生成，PCM 载荷为 16 kHz、单声道、
// S16_LE 的完整 20 ms（640 字节）帧。validate_event 先校验版本/标识，再校验载荷与终态，
// 失败返回结构化错误且不修改对象；调用方必须丢弃旧 generation 事件并保证 sequence 单调。
// JSON 元数据与二进制 PCM 分离，便于 ZeroMQ multipart 或 Fake JSON 传输；解码缺字段、未知
// 类型、错误码越界和长度不符均视为 kInvalidInput。值对象只读访问可并发，修改由调用方同步。
enum class DataEventType : std::uint8_t { kPartial, kFinal, kToken, kPcm, kDone, kError };
struct DataEvent {
  std::uint32_t version = 1;
  std::string request_id;
  std::string session_id;
  std::uint64_t generation = 0;
  std::uint64_t sequence = 0;
  DataEventType type = DataEventType::kError;
  std::string text;
  std::uint32_t frame_index = 0;
  bool end = false;
  domain::ErrorCode error_code = domain::ErrorCode::kNone;
  std::string message;
  std::size_t expected_pcm_bytes = 0;
  std::vector<std::uint8_t> pcm;
};
domain::OperationResult validate_event(const DataEvent&) noexcept;
domain::Result<std::string> encode_event_metadata(const DataEvent&);
domain::Result<DataEvent> decode_event_metadata(std::string_view);
domain::Result<std::vector<std::uint8_t>> encode_pcm_payload(const DataEvent&);
domain::Result<DataEvent> attach_pcm_payload(DataEvent,const std::vector<std::uint8_t>&);
}

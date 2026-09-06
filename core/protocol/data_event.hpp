// 数据面 v1 值对象：PCM（Pulse Code Modulation，脉冲编码调制）二进制与 JSON 头分开传递。
// 所有字段由对象拥有，输入字符串/字节只借用到调用结束；不创建线程、socket 或设备。
// 只读可并发，写入须外部同步。分配失败可抛 std::bad_alloc，栈对象清理且不修改原输入。
// request/session 遵守领域标识规则；generation 标识取消代际，sequence 在同一请求代内
// 从调用方指定的起点单调递增。排序与旧代过滤由接收方负责，纯值校验不维护历史。
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../domain/audio_frame.hpp"
#include "../domain/error.hpp"

namespace nexweave::protocol {
enum class DataEventType : std::uint8_t {
  kPartial,
  kFinal,
  kToken,
  kPcm,
  kDone,
  kError
};
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
  // 元数据声明的长度；本机构造时可为 0（由载荷推导），解码 PCM 头后必须为 640。
  std::size_t expected_pcm_bytes = 0;
  std::vector<std::uint8_t> pcm;
};

// 校验完整事件，先版本/归属/枚举，再终态/错误一致性，最后检查二进制长度。
// 未知枚举、错误类型不符、done 未标 end、非 PCM 携带字节或 PCM 非640字节都拒绝。
// 空或非法 request_id 返回 kMissingField，其余上述错误 kInvalidInput；成功不修改输入。
domain::OperationResult validate_event(const DataEvent& event);
// 编码只接受完整事件；PCM 元数据长度从实有字节数产生。非法 UTF-8 返回 kInvalidInput。
// v1 字段固定，缺字段/未知字段/负整数/布尔整数/浮点整数/溢出均不允许，扩展需新版本。
domain::Result<std::string> encode_event_metadata(const DataEvent& event);
// 成功头已通过所有公共校验和声明长度校验；PCM 的 pcm 仍为空，不能直接消费或编码，
// 必须经 attach_pcm_payload 才成为完整帧。非 PCM 成功时即为完整事件。
// JSON 格式或字段类型错误返回 kInvalidInput，领域校验保留其原错误码，失败无值。
domain::Result<DataEvent> decode_event_metadata(std::string_view input);
// 只接受已校验的完整 PCM 事件，返回自有字节副本；非 PCM 返回
// kInvalidInput，非法帧保留完整校验的原错误码。
domain::Result<std::vector<std::uint8_t>> encode_pcm_payload(const DataEvent& event);
// 按值接收头、复制 payload；成功返回完整事件，失败不改变调用方的头或字节。
// 声明长度非0时必须与载荷一致，再执行完整校验；取消/排序责任仍在接收方。
domain::Result<DataEvent> attach_pcm_payload(DataEvent event,
                                             const std::vector<std::uint8_t>& payload);
}  // namespace nexweave::protocol

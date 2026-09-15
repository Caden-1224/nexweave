// ZeroMQ 数据面 multipart 适配器：逐条传输音频输入事件与输出事件。
//
// 职责与边界
// ----------
// 本层只负责把 NexWeave 的 InputStreamEvent 与 protocol::DataEvent 编码成 ZeroMQ 双帧
// multipart 消息，并在接收侧按 v1 规则做元数据、二进制长度、流身份、代际和顺序校验。
// 它不跑会话、不做检索或推理，也不解释事件业务语义；音频帧合同、输入流合同和输出事件
// 合同分别由 domain::AudioFrame、runtime::InputStreamEvent 和 protocol::DataEvent 定义。
// ZeroMQ 类型只出现在实现文件中，公共接口不泄漏 socket/context。
//
// 线格式
// ------
// 一条数据消息固定为两帧：
//   part 1：UTF-8 JSON 元数据，字段集合固定，编码格式由本文件的公共编解码函数定义；
//   part 2：二进制载荷（PCM）；非 PCM 事件也必须发送一个零长度第二帧，以保持“一消息两帧”
//           的边界不随事件类型变化。
// 流开始、结束和取消是输入事件的语义，不是另一条 socket 命令；关闭是本地 socket 生命周期，
// 断连后由调用方重建通道。终态输出事件（done/error 且 end=true）由发送方恰好交付一次；
// 接收方需要在返回后自行缓存或查询，本层不保留已经交给调用方的事件。
//
// 线程与所有权
// ------------
// 每个 ZmqDataChannel 拥有自己的 ZeroMQ context 和 PAIR socket；它不在内部创建线程，send/
// receive 必须由同一个调用线程串行使用。socket 的 bind/connect/wait_ready/send/receive/
// close 都必须遵守这个单线程约定；需要双向并发时应由上层建立两个通道或显式加锁。
//
// deadline 与失败清理
// -------------------
// receive_* 接受相对 timeout；send_* 使用配置的发送超时。零长度超时意味着“只做一次非阻塞
// 尝试”，不会无限等待。接收超时不影响已经收到的完整消息；发送 multipart 中途失败会让该
// 通道进入不可用状态并关闭本地 socket，因为半条 multipart 无法可靠撤回。调用方失败后应
// close() 并按需要重建。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../protocol/data_event.hpp"
#include "../runtime/interaction_contract.hpp"

namespace nexweave::transport {

// 数据通道配置。所有容量与超时都是显式策略；非法值在构造时回退到默认值，也可用
// validate_zmq_data_config() 单独检查。HWM 是 ZeroMQ 单对端排队上限，不是应用层队列；
// 应用队列与统一背压由后续任务定义。max_metadata_bytes/max_payload_bytes 是本层直接拒绝
// 超大消息的上限，避免不可信对端用单个消息撑大内存。
struct ZmqDataConfig {
  std::size_t send_high_water_mark = 64;
  std::size_t receive_high_water_mark = 64;
  std::chrono::milliseconds send_timeout{500};
  std::size_t max_metadata_bytes = 64U * 1024U;
  std::size_t max_payload_bytes = 64U * 1024U;
};

// 校验数据通道配置。HWM、超时、元数据和载荷上限必须为正，且不得超出 int/int64 可选值范围。
domain::OperationResult validate_zmq_data_config(const ZmqDataConfig& config);

// 输入事件元数据解码结果。expected_pcm_bytes 来自线格式的 pcm_bytes 字段；事件本身不直接
// 携带二进制，接收方必须再调用 attach_input_pcm_payload() 才能得到完整可校验事件。
struct InputEventMetadata {
  runtime::InputStreamEvent event;
  std::size_t expected_pcm_bytes = 0;
};

// 输入事件元数据编码。成功返回严格 v1 JSON；失败不产生可发送值。编码会执行
// runtime::validate_input_event()，并额外要求“零有效样本的 end 不携带帧”，使线格式对
// start/frame/end 的二进制存在性没有歧义。
domain::Result<std::string> encode_input_event_metadata(
    const runtime::InputStreamEvent& event);

// 输入事件元数据解码。字段集合和类型必须严格匹配 v1；direction 必须为 "input"，
// kind 必须为 start/frame/end/cancel，pcm_bytes 与 kind/valid_samples 的关系必须成立。
domain::Result<InputEventMetadata> decode_input_event_metadata(
    std::string_view metadata);

// 把输入的二进制载荷附加到已解码元数据上。零长度载荷只能对应 expected_pcm_bytes==0；
// PCM 载荷必须恰好 640 字节，并按小端 S16_LE 转成 320 个 int16_t 样本。成功后再次执行
// runtime::validate_input_event()，因此缺帧、短帧、越界 valid_samples 不会进入上层。
domain::Result<runtime::InputStreamEvent> attach_input_pcm_payload(
    InputEventMetadata metadata, const std::vector<std::uint8_t>& payload);

// 把输入事件中的 PCM 样本编码为小端 S16_LE 字节；没有帧时返回空向量。仅供发送端与
// 测试复算使用，不修改输入事件。
domain::Result<std::vector<std::uint8_t>> encode_input_pcm_payload(
    const runtime::InputStreamEvent& event);

// 通道级可观察账目。单线程通道内的计数只增不减；它们记录的是本适配器看到的事实，不是
// 对端成功处理或端到端投递证明。所有容量、超时和拒绝都通过这里的计数器与结构化返回暴露。
struct ZmqDataStats {
  std::uint64_t sent_input = 0;
  std::uint64_t sent_output = 0;
  std::uint64_t received_input = 0;
  std::uint64_t received_output = 0;
  std::uint64_t rejected = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t send_failures = 0;
};

// 点对点数据通道。一个实例在同一线程内串行使用；bind()/connect() 只建立一端，wait_ready()
// 完成显式握手后 ready() 为真，send_* 与 receive_* 才被允许。通道可同时收发输入事件和
// 输出事件，但输入顺序校验与输出顺序校验各自维护独立状态。
class ZmqDataChannel final {
 public:
  explicit ZmqDataChannel(ZmqDataConfig config = {});
  ~ZmqDataChannel();

  ZmqDataChannel(const ZmqDataChannel&) = delete;
  ZmqDataChannel& operator=(const ZmqDataChannel&) = delete;

  // 绑定/连接 PAIR socket。一个实例只允许成功建立一次；失败时不留下可用端点。bind 成功后
  // bound_endpoint() 返回系统分配或显式给出的实际端点；connect 失败返回结构化错误。
  domain::OperationResult bind(const std::string& endpoint);
  domain::OperationResult connect(const std::string& endpoint);

  // 显式连接就绪握手。bind 端等待连接端的 hello 后回 ready；connect 端发送 hello 并等待
  // ready。必须在 send_*/receive_* 之前成功调用；重复调用在已 ready 时成功返回。
  domain::OperationResult wait_ready(std::chrono::milliseconds timeout);

  bool ready() const noexcept;
  // 绑定成功后的实际端点；未绑定或已关闭时为空串。返回副本。
  std::string bound_endpoint() const;

  // 发送输入/输出事件。输入元数据编码和输出事件编码都先通过 v1 校验；multipart 中途失败
  // 会关闭本地通道并返回错误。成功仅表示消息已交给 ZeroMQ，不代表对端已消费或落库。
  domain::OperationResult send_input(const runtime::InputStreamEvent& event);
  domain::OperationResult send_output(const protocol::DataEvent& event);

  // 接收并验证下一条输入/输出事件。timeout 是相对等待预算；超时返回 kTimeout，不消费
  // 已经到达的完整消息之外的数据。输入会额外检查流身份、代际、序号连续和结束后写入；
  // 输出会检查同一身份下序号严格递增且终态只出现一次。
  domain::Result<runtime::InputStreamEvent> receive_input(
      std::chrono::milliseconds timeout);
  domain::Result<protocol::DataEvent> receive_output(
      std::chrono::milliseconds timeout);

  // 关闭本地 socket，幂等、不抛异常。已经收到的完整消息不受影响；未发送完成的 multipart
  // 不会补发。关闭后可再次 bind()/connect() 建立新一轮通道。
  void close() noexcept;

  // 返回当前通道的传输账目副本。调用不阻塞、不改变计数，必须与 send_*/receive_* 同线程调用。
  ZmqDataStats stats() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::transport

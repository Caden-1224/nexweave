#include "../test_support.hpp"

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "zmq_data.hpp"

using namespace nexweave;
using namespace std::chrono_literals;

namespace {

using transport::InputEventMetadata;
using transport::ZmqDataChannel;
using transport::ZmqDataConfig;

domain::AudioFrame Frame(std::int16_t value) {
  domain::AudioFrame frame;
  frame.samples.assign(domain::kAudioFrameSamples, value);
  return frame;
}

runtime::InputStreamEvent InputEvent(const std::string& stream_id,
                                     std::uint64_t generation,
                                     std::uint64_t sequence,
                                     runtime::InputEventKind kind) {
  runtime::InputStreamEvent event;
  event.stream_id = stream_id;
  event.generation = generation;
  event.sequence = sequence;
  event.kind = kind;
  return event;
}

runtime::InputStreamEvent InputFrame(const std::string& stream_id,
                                     std::uint64_t generation,
                                     std::uint64_t sequence,
                                     std::int16_t value) {
  auto event = InputEvent(stream_id, generation, sequence,
                          runtime::InputEventKind::kFrame);
  event.frame = Frame(value);
  event.valid_samples = domain::kAudioFrameSamples;
  return event;
}

runtime::InputStreamEvent InputEnd(const std::string& stream_id,
                                   std::uint64_t generation,
                                   std::uint64_t sequence) {
  return InputEvent(stream_id, generation, sequence,
                    runtime::InputEventKind::kEnd);
}

protocol::DataEvent TokenEvent(const std::string& request_id,
                               const std::string& session_id,
                               std::uint64_t generation,
                               std::uint64_t sequence,
                               const std::string& text) {
  protocol::DataEvent event;
  event.request_id = request_id;
  event.session_id = session_id;
  event.generation = generation;
  event.sequence = sequence;
  event.type = protocol::DataEventType::kToken;
  event.text = text;
  return event;
}

protocol::DataEvent PcmEvent(const std::string& request_id,
                             const std::string& session_id,
                             std::uint64_t generation,
                             std::uint64_t sequence,
                             std::uint8_t fill) {
  protocol::DataEvent event;
  event.request_id = request_id;
  event.session_id = session_id;
  event.generation = generation;
  event.sequence = sequence;
  event.type = protocol::DataEventType::kPcm;
  event.frame_index = 0;
  event.pcm.assign(domain::kAudioFrameBytes, fill);
  return event;
}

protocol::DataEvent TerminalEvent(const std::string& request_id,
                                  const std::string& session_id,
                                  std::uint64_t generation,
                                  std::uint64_t sequence,
                                  protocol::DataEventType type) {
  protocol::DataEvent event;
  event.request_id = request_id;
  event.session_id = session_id;
  event.generation = generation;
  event.sequence = sequence;
  event.type = type;
  event.end = true;
  if (type == protocol::DataEventType::kError) {
    event.error_code = domain::ErrorCode::kCancelled;
    event.message = "cancelled";
  } else {
    event.message = "state=done";
  }
  return event;
}

// 两端就绪握手需要并发等待：连接端在后台线程发送 hello，绑定端在主线程回应 ready。
// 两个通道对象各自只被一个线程触碰，因此符合“单通道单线程串行”的契约。
struct ChannelPair {
  ZmqDataChannel binder;
  ZmqDataChannel connector;

  bool Start(std::chrono::milliseconds timeout = 2000ms) {
    const auto bound = binder.bind("tcp://127.0.0.1:*");
    if (!bound.ok()) {
      return false;
    }
    const auto connected = connector.connect(binder.bound_endpoint());
    if (!connected.ok()) {
      return false;
    }
    domain::OperationResult connector_result;
    std::thread connector_thread([this, timeout, &connector_result] {
      connector_result = connector.wait_ready(timeout);
    });
    const auto binder_result = binder.wait_ready(timeout);
    connector_thread.join();
    return binder_result.ok() && connector_result.ok();
  }
};

// 保护不变量：输入流的 start/frame/end 顺序、流身份、代际和二进制长度在接收侧被共同
// 校验；结束后重复 end 或继续送 frame 得到确定错误，新一轮 start 可以重新开始。
void TestInputStreamLifecycle() {
  ChannelPair pair;
  CHECK(pair.Start());

  const auto start = InputEvent("stream-1", 3, 0, runtime::InputEventKind::kStart);
  CHECK(pair.connector.send_input(start).ok());
  const auto received_start = pair.binder.receive_input(1000ms);
  CHECK(received_start.ok());
  CHECK(received_start.value->stream_id == "stream-1");
  CHECK(received_start.value->generation == 3);
  CHECK(received_start.value->sequence == 0);

  CHECK(pair.connector.send_input(InputFrame("stream-1", 3, 1, 123)).ok());
  const auto received_frame = pair.binder.receive_input(1000ms);
  CHECK(received_frame.ok());
  CHECK(received_frame.value->kind == runtime::InputEventKind::kFrame);
  CHECK(received_frame.value->frame.has_value());
  CHECK((*received_frame.value->frame).samples[0] == 123);
  CHECK((*received_frame.value->frame).samples[319] == 123);

  CHECK(pair.connector.send_input(InputEnd("stream-1", 3, 2)).ok());
  const auto received_end = pair.binder.receive_input(1000ms);
  CHECK(received_end.ok());
  CHECK(received_end.value->kind == runtime::InputEventKind::kEnd);

  CHECK(pair.connector.send_input(InputEnd("stream-1", 3, 3)).ok());
  const auto duplicate_end = pair.binder.receive_input(1000ms);
  CHECK(!duplicate_end.ok());
  CHECK(duplicate_end.error.code == domain::ErrorCode::kAlreadyCompleted);

  CHECK(pair.connector.send_input(InputFrame("stream-1", 3, 4, 7)).ok());
  const auto frame_after_end = pair.binder.receive_input(1000ms);
  CHECK(!frame_after_end.ok());
  CHECK(frame_after_end.error.code == domain::ErrorCode::kAlreadyCompleted);

  CHECK(pair.connector.send_input(InputEvent("stream-2", 4, 0,
                                             runtime::InputEventKind::kStart))
            .ok());
  const auto new_start = pair.binder.receive_input(1000ms);
  CHECK(new_start.ok());
  CHECK(new_start.value->stream_id == "stream-2");

  // 零帧结束：start 后直接 end，不带任何 PCM 或有效样本。
  CHECK(pair.connector.send_input(InputEnd("stream-2", 4, 1)).ok());
  const auto zero_frame_end = pair.binder.receive_input(1000ms);
  CHECK(zero_frame_end.ok());
  CHECK(zero_frame_end.value->kind == runtime::InputEventKind::kEnd);
  CHECK(!zero_frame_end.value->frame.has_value());

  const auto sender_stats = pair.connector.stats();
  const auto receiver_stats = pair.binder.stats();
  CHECK(sender_stats.sent_input == 7);
  CHECK(receiver_stats.received_input == 5);
  CHECK(receiver_stats.rejected >= 2);
}

// 保护不变量：帧序号必须连续、stream_id 与 generation 必须与 start 匹配；错误归属或
// 乱序不推进接收状态，后续正确的下一帧仍可继续。
void TestInputSequenceAndOwnership() {
  ChannelPair pair;
  CHECK(pair.Start());

  CHECK(pair.connector.send_input(InputEvent("stream-a", 7, 0,
                                             runtime::InputEventKind::kStart))
            .ok());
  CHECK(pair.binder.receive_input(1000ms).ok());

  CHECK(pair.connector.send_input(InputFrame("stream-a", 7, 5, 1)).ok());
  const auto wrong_sequence = pair.binder.receive_input(1000ms);
  CHECK(!wrong_sequence.ok());
  CHECK(wrong_sequence.error.code == domain::ErrorCode::kInvalidInput);

  CHECK(pair.connector.send_input(InputFrame("stream-b", 7, 1, 1)).ok());
  const auto wrong_stream = pair.binder.receive_input(1000ms);
  CHECK(!wrong_stream.ok());
  CHECK(wrong_stream.error.code == domain::ErrorCode::kInvalidInput);

  CHECK(pair.connector.send_input(InputFrame("stream-a", 8, 1, 1)).ok());
  const auto wrong_generation = pair.binder.receive_input(1000ms);
  CHECK(!wrong_generation.ok());
  CHECK(wrong_generation.error.code == domain::ErrorCode::kInvalidInput);

  CHECK(pair.connector.send_input(InputFrame("stream-a", 7, 1, 2)).ok());
  const auto correct = pair.binder.receive_input(1000ms);
  CHECK(correct.ok());
  CHECK(correct.value->sequence == 1);
}

// 保护不变量：元数据声明长度与实际上载载荷必须一致；缺帧、短帧和零样本 end 携带帧
// 都在构造完整事件前失败，不会把半帧交给上游。
void TestInputPayloadValidation() {
  const auto frame = InputFrame("stream-payload", 1, 1, 9);
  const auto metadata = transport::encode_input_event_metadata(frame);
  CHECK(metadata.ok());
  const auto decoded = transport::decode_input_event_metadata(*metadata.value);
  CHECK(decoded.ok());
  CHECK(decoded.value->expected_pcm_bytes == domain::kAudioFrameBytes);

  CHECK(!transport::attach_input_pcm_payload(*decoded.value, {}).ok());
  std::vector<std::uint8_t> short_payload(10, 0);
  CHECK(!transport::attach_input_pcm_payload(*decoded.value, short_payload).ok());
  const auto payload = transport::encode_input_pcm_payload(frame);
  CHECK(payload.ok());
  CHECK(payload.value->size() == domain::kAudioFrameBytes);
  const auto attached =
      transport::attach_input_pcm_payload(*decoded.value, *payload.value);
  CHECK(attached.ok());
  CHECK(attached.value->frame.has_value());

  auto zero_end = InputEnd("stream-payload", 1, 2);
  zero_end.frame = Frame(1);
  CHECK(!transport::encode_input_event_metadata(zero_end).ok());

  const std::string unknown_field =
      "{\"version\":1,\"direction\":\"input\",\"kind\":\"start\","
      "\"stream_id\":\"s\",\"generation\":0,\"sequence\":0,"
      "\"valid_samples\":0,\"pcm_bytes\":0,\"extra\":1}";
  CHECK(!transport::decode_input_event_metadata(unknown_field).ok());
  const std::string missing_field =
      "{\"version\":1,\"direction\":\"input\",\"kind\":\"start\","
      "\"stream_id\":\"s\",\"generation\":0,\"sequence\":0,"
      "\"valid_samples\":0}";
  CHECK(!transport::decode_input_event_metadata(missing_field).ok());
}

// 保护不变量：输出事件携带元数据与二进制；同一身份下 sequence 必须递增，终态之后
// 不得再交付事件。PCM 载荷必须恰好一帧，非 PCM 事件不得夹带二进制。
void TestOutputEventsAndTerminal() {
  ChannelPair pair;
  CHECK(pair.Start());

  CHECK(pair.connector.send_output(
                   TokenEvent("req-1", "sess-1", 1, 1, "hello"))
            .ok());
  const auto token = pair.binder.receive_output(1000ms);
  CHECK(token.ok());
  CHECK(token.value->type == protocol::DataEventType::kToken);
  CHECK(token.value->text == "hello");

  CHECK(pair.connector.send_output(PcmEvent("req-1", "sess-1", 1, 2, 0x5A)).ok());
  const auto pcm = pair.binder.receive_output(1000ms);
  CHECK(pcm.ok());
  CHECK(pcm.value->type == protocol::DataEventType::kPcm);
  CHECK(pcm.value->pcm.size() == domain::kAudioFrameBytes);
  CHECK(pcm.value->pcm[0] == 0x5A);

  CHECK(pair.connector.send_output(
                   TerminalEvent("req-1", "sess-1", 1, 3,
                                 protocol::DataEventType::kDone))
            .ok());
  const auto done = pair.binder.receive_output(1000ms);
  CHECK(done.ok());
  CHECK(done.value->type == protocol::DataEventType::kDone);
  CHECK(done.value->end);

  CHECK(pair.connector.send_output(
                   TokenEvent("req-1", "sess-1", 1, 4, "late"))
            .ok());
  const auto after_terminal = pair.binder.receive_output(1000ms);
  CHECK(!after_terminal.ok());
  CHECK(after_terminal.error.code == domain::ErrorCode::kAlreadyCompleted);

  CHECK(pair.connector.send_output(
                   TokenEvent("req-2", "sess-1", 2, 1, "new"))
            .ok());
  const auto new_stream = pair.binder.receive_output(1000ms);
  CHECK(new_stream.ok());
  CHECK(new_stream.value->request_id == "req-2");

  CHECK(pair.connector.send_output(
                   TokenEvent("req-2", "sess-1", 2, 1, "out-of-order"))
            .ok());
  const auto out_of_order = pair.binder.receive_output(1000ms);
  CHECK(!out_of_order.ok());
  CHECK(out_of_order.error.code == domain::ErrorCode::kInvalidInput);

  CHECK(pair.connector.send_output(
                   TokenEvent("req-2", "sess-1", 1, 2, "stale-generation"))
            .ok());
  const auto stale_generation = pair.binder.receive_output(1000ms);
  CHECK(!stale_generation.ok());
  CHECK(stale_generation.error.code == domain::ErrorCode::kCancelled);

  const auto sender_stats = pair.connector.stats();
  const auto receiver_stats = pair.binder.stats();
  CHECK(sender_stats.sent_output == 7);
  CHECK(receiver_stats.received_output == 4);
  CHECK(receiver_stats.rejected >= 3);
}

// 保护不变量：未 ready 的通道拒绝数据；连接端在无对端时 wait_ready 以 kTimeout 收敛，
// 不无限重试；close 幂等并且之后可以重新 bind/connect。
void TestDeadlineAndClose() {
  ZmqDataChannel connector;
  CHECK(connector.connect("tcp://127.0.0.1:1").ok());
  const auto timed_out = connector.wait_ready(150ms);
  CHECK(!timed_out.ok());
  CHECK(timed_out.error.code == domain::ErrorCode::kTimeout);

  const auto before_ready = connector.send_input(
      InputEvent("stream-x", 0, 0, runtime::InputEventKind::kStart));
  CHECK(!before_ready.ok());
  connector.close();
  connector.close();

  ZmqDataChannel binder;
  CHECK(binder.bind("tcp://127.0.0.1:*").ok());
  CHECK(!binder.bound_endpoint().empty());
  binder.close();
  CHECK(binder.bound_endpoint().empty());
}

}  // namespace

int main() {
  TestInputStreamLifecycle();
  TestInputSequenceAndOwnership();
  TestInputPayloadValidation();
  TestOutputEventsAndTerminal();
  TestDeadlineAndClose();
}

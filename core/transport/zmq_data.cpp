#include "zmq_data.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include "../protocol/json_fields.hpp"

namespace nexweave::transport {
namespace {

using domain::ErrorCode;
using domain::OperationResult;
using protocol::DataEvent;
using runtime::InputEventKind;
using runtime::InputStreamEvent;

constexpr char kHelloMagic[] = "NEXWEAVE-DATA-HELLO-v1";
constexpr char kReadyMagic[] = "NEXWEAVE-DATA-READY-v1";

int socket_timeout_millis(std::chrono::milliseconds value) {
  if (value.count() < 1) {
    return 1;
  }
  if (value.count() > std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(value.count());
}

const char* input_kind_name(InputEventKind kind) noexcept {
  switch (kind) {
    case InputEventKind::kStart:
      return "start";
    case InputEventKind::kFrame:
      return "frame";
    case InputEventKind::kEnd:
      return "end";
    case InputEventKind::kCancel:
      return "cancel";
  }
  return "";
}

bool parse_input_kind(const std::string& text, InputEventKind& kind) noexcept {
  if (text == "start") {
    kind = InputEventKind::kStart;
    return true;
  }
  if (text == "frame") {
    kind = InputEventKind::kFrame;
    return true;
  }
  if (text == "end") {
    kind = InputEventKind::kEnd;
    return true;
  }
  if (text == "cancel") {
    kind = InputEventKind::kCancel;
    return true;
  }
  return false;
}

OperationResult validate_input_metadata(const InputStreamEvent& event,
                                        std::size_t expected_pcm_bytes) {
  if (event.version != 1 || event.stream_id.empty() || input_kind_name(event.kind)[0] == '\0') {
    return OperationResult::failure(ErrorCode::kInvalidInput, "输入事件版本、流标识或类型无效");
  }
  if (event.kind == InputEventKind::kStart || event.kind == InputEventKind::kCancel) {
    if (event.valid_samples != 0 || expected_pcm_bytes != 0) {
      return OperationResult::failure(ErrorCode::kInvalidInput,
                                      "start/cancel 不得携带有效样本或 PCM");
    }
    return OperationResult::success();
  }
  if (event.kind == InputEventKind::kFrame) {
    if (event.valid_samples != domain::kAudioFrameSamples ||
        expected_pcm_bytes != domain::kAudioFrameBytes) {
      return OperationResult::failure(ErrorCode::kInvalidInput,
                                      "frame 必须携带完整 320 样本 PCM");
    }
    return OperationResult::success();
  }
  // end：允许零样本结束；带样本时必须是一帧补零后的 1..320 个有效样本和 640 字节 PCM。
  if (event.valid_samples > domain::kAudioFrameSamples) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "end 的有效样本数越界");
  }
  if (event.valid_samples == 0) {
    if (expected_pcm_bytes != 0) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "零样本 end 不得携带 PCM");
    }
  } else if (expected_pcm_bytes != domain::kAudioFrameBytes) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "带样本 end 必须携带完整 PCM");
  }
  return OperationResult::success();
}

std::vector<std::uint8_t> encode_little_endian_samples(
    const std::vector<std::int16_t>& samples) {
  std::vector<std::uint8_t> bytes(samples.size() * sizeof(std::int16_t));
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const std::uint16_t value = static_cast<std::uint16_t>(samples[index]);
    bytes[index * 2U] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[index * 2U + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  }
  return bytes;
}

std::vector<std::int16_t> decode_little_endian_samples(
    const std::vector<std::uint8_t>& payload) {
  std::vector<std::int16_t> samples;
  samples.reserve(payload.size() / 2U);
  for (std::size_t index = 0; index + 1U < payload.size(); index += 2U) {
    const std::uint16_t value = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(payload[index]) |
        (static_cast<std::uint16_t>(payload[index + 1U]) << 8U));
    samples.push_back(static_cast<std::int16_t>(value));
  }
  return samples;
}

}  // namespace

domain::OperationResult validate_zmq_data_config(const ZmqDataConfig& config) {
  if (config.send_high_water_mark == 0 || config.receive_high_water_mark == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 数据水位必须为正");
  }
  if (config.send_timeout.count() <= 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 数据发送预算必须为正");
  }
  if (config.max_metadata_bytes == 0 || config.max_payload_bytes == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 数据消息上限必须为正");
  }
  const auto int64_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (config.max_metadata_bytes > int64_max || config.max_payload_bytes > int64_max) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 数据消息上限超出范围");
  }
  if (config.send_high_water_mark >
          static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      config.receive_high_water_mark >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "ZeroMQ 数据水位超出范围");
  }
  return OperationResult::success();
}

domain::Result<std::vector<std::uint8_t>> encode_input_pcm_payload(
    const InputStreamEvent& event) {
  const auto validation = runtime::validate_input_event(event);
  if (!validation.ok()) {
    return domain::Result<std::vector<std::uint8_t>>::failure(
        validation.error.code, validation.error.message);
  }
  if (!event.frame.has_value()) {
    return domain::Result<std::vector<std::uint8_t>>::success({});
  }
  return domain::Result<std::vector<std::uint8_t>>::success(
      encode_little_endian_samples(event.frame->samples));
}

domain::Result<std::string> encode_input_event_metadata(
    const InputStreamEvent& event) {
  const auto validation = runtime::validate_input_event(event);
  if (!validation.ok()) {
    return domain::Result<std::string>::failure(validation.error.code,
                                                validation.error.message);
  }
  if (event.kind == InputEventKind::kEnd && event.valid_samples == 0 &&
      event.frame.has_value()) {
    return domain::Result<std::string>::failure(
        ErrorCode::kInvalidInput, "零有效样本的 end 不得携带帧");
  }

  nlohmann::json metadata;
  metadata["version"] = event.version;
  metadata["direction"] = "input";
  metadata["kind"] = input_kind_name(event.kind);
  metadata["stream_id"] = event.stream_id;
  metadata["generation"] = event.generation;
  metadata["sequence"] = event.sequence;
  metadata["valid_samples"] = event.valid_samples;
  metadata["pcm_bytes"] =
      event.frame.has_value() ? domain::kAudioFrameBytes : std::size_t{0};
  return protocol::detail::dump(metadata);
}

domain::Result<InputEventMetadata> decode_input_event_metadata(
    std::string_view metadata) {
  return protocol::detail::decode<InputEventMetadata>(
      metadata,
      {"version", "direction", "kind", "stream_id", "generation", "sequence",
       "valid_samples", "pcm_bytes"},
      [](const protocol::detail::Json& json) {
        if (json.at("direction").get<std::string>() != "input") {
          throw std::invalid_argument("direction 必须为 input");
        }
        InputEventMetadata decoded;
        decoded.event.version =
            protocol::detail::integer<std::uint32_t>(json, "version");
        if (!parse_input_kind(json.at("kind").get<std::string>(),
                              decoded.event.kind)) {
          throw std::invalid_argument("未知输入事件类型");
        }
        decoded.event.stream_id = json.at("stream_id").get<std::string>();
        decoded.event.generation =
            protocol::detail::integer<std::uint64_t>(json, "generation");
        decoded.event.sequence =
            protocol::detail::integer<std::uint64_t>(json, "sequence");
        decoded.event.valid_samples =
            protocol::detail::integer<std::size_t>(json, "valid_samples");
        decoded.expected_pcm_bytes =
            protocol::detail::integer<std::size_t>(json, "pcm_bytes");
        const auto validation =
            validate_input_metadata(decoded.event, decoded.expected_pcm_bytes);
        if (!validation.ok()) {
          return domain::Result<InputEventMetadata>::failure(
              validation.error.code, validation.error.message);
        }
        return domain::Result<InputEventMetadata>::success(std::move(decoded));
      });
}

domain::Result<InputStreamEvent> attach_input_pcm_payload(
    InputEventMetadata metadata, const std::vector<std::uint8_t>& payload) {
  if (metadata.expected_pcm_bytes == 0) {
    if (!payload.empty()) {
      return domain::Result<InputStreamEvent>::failure(
          ErrorCode::kInvalidInput, "元数据声明零长度 PCM 但收到非空载荷");
    }
    metadata.event.frame.reset();
    const auto validation = runtime::validate_input_event(metadata.event);
    if (!validation.ok()) {
      return domain::Result<InputStreamEvent>::failure(
          validation.error.code, validation.error.message);
    }
    return domain::Result<InputStreamEvent>::success(std::move(metadata.event));
  }
  if (metadata.expected_pcm_bytes != domain::kAudioFrameBytes ||
      payload.size() != domain::kAudioFrameBytes) {
    return domain::Result<InputStreamEvent>::failure(
        ErrorCode::kInvalidInput, "输入 PCM 声明长度或实际长度不是 640 字节");
  }
  const auto samples = decode_little_endian_samples(payload);
  const auto frame = domain::AudioFrame::from_samples(samples);
  if (!frame.ok()) {
    return domain::Result<InputStreamEvent>::failure(
        ErrorCode::kInvalidInput, "输入 PCM 不能构造合法音频帧");
  }
  metadata.event.frame = frame.value;
  const auto validation = runtime::validate_input_event(metadata.event);
  if (!validation.ok()) {
    return domain::Result<InputStreamEvent>::failure(
        validation.error.code, validation.error.message);
  }
  return domain::Result<InputStreamEvent>::success(std::move(metadata.event));
}

struct ZmqDataChannel::Impl {
  explicit Impl(ZmqDataConfig config_value) : config(std::move(config_value)) {}

  enum class Role { kNone, kBinder, kConnector };

  struct InputSequence {
    bool active = false;
    bool ended = false;
    std::string stream_id;
    std::uint64_t generation = 0;
    std::uint64_t next_sequence = 0;

    void reset() {
      active = false;
      ended = false;
      stream_id.clear();
      generation = 0;
      next_sequence = 0;
    }

    OperationResult accept(const InputStreamEvent& event) {
      if (event.kind == InputEventKind::kStart) {
        if (active && !ended) {
          return OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                          "输入流已经开始");
        }
        if (event.sequence != 0) {
          return OperationResult::failure(ErrorCode::kInvalidInput,
                                          "输入流 start 的 sequence 必须为 0");
        }
        active = true;
        ended = false;
        stream_id = event.stream_id;
        generation = event.generation;
        next_sequence = 1;
        return OperationResult::success();
      }

      if (!active) {
        return OperationResult::failure(ErrorCode::kInvalidInput, "输入流尚未 start");
      }
      if (ended) {
        return OperationResult::failure(ErrorCode::kAlreadyCompleted, "输入流已经结束");
      }
      if (event.stream_id != stream_id || event.generation != generation) {
        return OperationResult::failure(ErrorCode::kInvalidInput,
                                        "输入流身份或代际不匹配");
      }
      if (event.sequence != next_sequence) {
        return OperationResult::failure(ErrorCode::kInvalidInput, "输入序号乱序或缺帧");
      }
      ++next_sequence;
      if (event.kind == InputEventKind::kEnd || event.kind == InputEventKind::kCancel) {
        ended = true;
      }
      return OperationResult::success();
    }
  };

  struct OutputSequence {
    bool has_key = false;
    bool ended = false;
    std::string request_id;
    std::string session_id;
    std::uint64_t generation = 0;
    std::uint64_t last_sequence = 0;

    void reset() { *this = OutputSequence{}; }

    OperationResult accept(const DataEvent& event) {
      if (has_key && event.request_id == request_id &&
          event.session_id == session_id) {
        if (event.generation < generation) {
          // 旧代际的迟到事件即使序号更大也必须被拒绝；这里只表达传输层的代际过滤，
          // Session/网关仍负责决定何时推进代际并丢弃旧结果。
          return OperationResult::failure(ErrorCode::kCancelled, "输出事件属于旧代际");
        }
        if (event.generation == generation) {
          if (ended) {
            return OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                            "输出流已经交付终态");
          }
          if (event.sequence <= last_sequence) {
            return OperationResult::failure(ErrorCode::kInvalidInput, "输出序号乱序");
          }
          last_sequence = event.sequence;
          ended = event.end;
          return OperationResult::success();
        }
        // 同一 request/session 的更高代际：开始一轮新的输出流。
      }

      has_key = true;
      ended = false;
      request_id = event.request_id;
      session_id = event.session_id;
      generation = event.generation;
      last_sequence = event.sequence;
      ended = event.end;
      return OperationResult::success();
    }
  };

  ZmqDataConfig config;
  zmq::context_t context{1};
  std::unique_ptr<zmq::socket_t> socket;
  Role role = Role::kNone;
  bool ready = false;
  std::string bound_endpoint;
  InputSequence input_sequence;
  OutputSequence output_sequence;
  ZmqDataStats counters;

  OperationResult make_socket() {
    try {
      socket = std::make_unique<zmq::socket_t>(context, zmq::socket_type::pair);
      socket->set(zmq::sockopt::linger, 0);
      socket->set(zmq::sockopt::sndhwm, static_cast<int>(config.send_high_water_mark));
      socket->set(zmq::sockopt::rcvhwm,
                  static_cast<int>(config.receive_high_water_mark));
      const auto max_message = std::max(config.max_metadata_bytes,
                                        config.max_payload_bytes);
      socket->set(zmq::sockopt::maxmsgsize,
                  static_cast<std::int64_t>(max_message));
      // immediate=1：连接尚未建立时不做无界排队，发送以 EAGAIN 暴露“未就绪”。
      socket->set(zmq::sockopt::immediate, 1);
      return OperationResult::success();
    } catch (const zmq::error_t& error) {
      socket.reset();
      return OperationResult::failure(ErrorCode::kBackendFailure, error.what());
    }
  }

  void close_socket() noexcept {
    try {
      socket.reset();
    } catch (...) {
      // 析构已尽力回收；不把清理异常传播给关闭路径。
    }
    role = Role::kNone;
    ready = false;
    bound_endpoint.clear();
    input_sequence.reset();
    output_sequence.reset();
  }

  OperationResult send_frame(std::string_view frame, bool more,
                             std::chrono::milliseconds timeout) {
    if (!socket) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "数据通道未建立");
    }
    try {
      socket->set(zmq::sockopt::sndtimeo, socket_timeout_millis(timeout));
      const auto sent = socket->send(
          zmq::buffer(frame.data(), frame.size()),
          more ? zmq::send_flags::sndmore : zmq::send_flags::none);
      if (!sent.has_value()) {
        return OperationResult::failure(ErrorCode::kTimeout, "ZeroMQ 发送超时");
      }
      return OperationResult::success();
    } catch (const zmq::error_t& error) {
      if (error.num() == EAGAIN || error.num() == EINTR) {
        return OperationResult::failure(ErrorCode::kTimeout, error.what());
      }
      return OperationResult::failure(ErrorCode::kBackendFailure, error.what());
    }
  }

  OperationResult send_binary(const std::vector<std::uint8_t>& payload, bool more,
                              std::chrono::milliseconds timeout) {
    if (!socket) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "数据通道未建立");
    }
    try {
      socket->set(zmq::sockopt::sndtimeo, socket_timeout_millis(timeout));
      const auto sent = socket->send(
          zmq::buffer(payload),
          more ? zmq::send_flags::sndmore : zmq::send_flags::none);
      if (!sent.has_value()) {
        return OperationResult::failure(ErrorCode::kTimeout, "ZeroMQ 发送超时");
      }
      return OperationResult::success();
    } catch (const zmq::error_t& error) {
      if (error.num() == EAGAIN || error.num() == EINTR) {
        return OperationResult::failure(ErrorCode::kTimeout, error.what());
      }
      return OperationResult::failure(ErrorCode::kBackendFailure, error.what());
    }
  }

  OperationResult receive_message(zmq::message_t& message,
                                  std::chrono::milliseconds timeout) {
    if (!socket) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "数据通道未建立");
    }
    try {
      socket->set(zmq::sockopt::rcvtimeo, socket_timeout_millis(timeout));
      const auto received = socket->recv(message, zmq::recv_flags::none);
      if (!received.has_value()) {
        return OperationResult::failure(ErrorCode::kTimeout, "ZeroMQ 接收超时");
      }
      return OperationResult::success();
    } catch (const zmq::error_t& error) {
      if (error.num() == EAGAIN || error.num() == EINTR) {
        return OperationResult::failure(ErrorCode::kTimeout, error.what());
      }
      return OperationResult::failure(ErrorCode::kBackendFailure, error.what());
    }
  }

  OperationResult send_single_frame(std::string_view frame,
                                    std::chrono::milliseconds timeout) {
    return send_frame(frame, false, timeout);
  }

  OperationResult receive_single_frame(std::string& frame,
                                       std::chrono::milliseconds timeout) {
    zmq::message_t message;
    const auto received = receive_message(message, timeout);
    if (!received.ok()) {
      return received;
    }
    if (message.more()) {
      return OperationResult::failure(ErrorCode::kInvalidInput,
                                      "就绪握手必须是单帧消息");
    }
    frame.assign(message.data<char>(), message.size());
    return OperationResult::success();
  }

  OperationResult send_multipart(const std::string& metadata,
                                 const std::vector<std::uint8_t>& payload) {
    const auto deadline = std::chrono::steady_clock::now() + config.send_timeout;
    auto remaining = [&deadline]() {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return std::chrono::milliseconds{0};
      }
      return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    };

    const auto metadata_frame = send_frame(metadata, true, remaining());
    if (!metadata_frame.ok()) {
      close_socket();
      return metadata_frame;
    }
    const auto payload_frame = send_binary(payload, false, remaining());
    if (!payload_frame.ok()) {
      close_socket();
      return payload_frame;
    }
    return OperationResult::success();
  }

  OperationResult receive_multipart(std::string& metadata,
                                    std::vector<std::uint8_t>& payload,
                                    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto remaining = [&deadline]() {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return std::chrono::milliseconds{0};
      }
      return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    };

    zmq::message_t metadata_message;
    const auto metadata_received = receive_message(metadata_message, remaining());
    if (!metadata_received.ok()) {
      return metadata_received;
    }
    if (metadata_message.size() > config.max_metadata_bytes) {
      close_socket();
      return OperationResult::failure(ErrorCode::kInvalidInput, "元数据超过上限");
    }
    if (!metadata_message.more()) {
      return OperationResult::failure(ErrorCode::kInvalidInput,
                                      "数据消息必须是两帧 multipart");
    }
    metadata.assign(metadata_message.data<char>(), metadata_message.size());

    zmq::message_t payload_message;
    const auto payload_received = receive_message(payload_message, remaining());
    if (!payload_received.ok()) {
      close_socket();
      return payload_received;
    }
    if (payload_message.size() > config.max_payload_bytes) {
      close_socket();
      return OperationResult::failure(ErrorCode::kInvalidInput, "载荷超过上限");
    }
    if (payload_message.more()) {
      return OperationResult::failure(ErrorCode::kInvalidInput,
                                      "数据消息超过两帧");
    }
    if (payload_message.size() == 0) {
      payload.clear();
    } else {
      payload.assign(payload_message.data<std::uint8_t>(),
                     payload_message.data<std::uint8_t>() + payload_message.size());
    }
    return OperationResult::success();
  }

  OperationResult wait_ready(std::chrono::milliseconds timeout) {
    if (ready) {
      return OperationResult::success();
    }
    if (!socket || role == Role::kNone) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "数据通道尚未 bind/connect");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto remaining = [&deadline]() {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return std::chrono::milliseconds{0};
      }
      return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    };

    if (role == Role::kBinder) {
      while (remaining().count() >= 0) {
        std::string frame;
        const auto received = receive_single_frame(frame, remaining());
        if (!received.ok()) {
          if (received.error.code == ErrorCode::kTimeout) {
            return OperationResult::failure(ErrorCode::kTimeout, "等待数据端 hello 超时");
          }
          return received;
        }
        if (frame != kHelloMagic) {
          return OperationResult::failure(ErrorCode::kInvalidInput,
                                          "就绪前收到非法帧");
        }
        const auto sent = send_single_frame(kReadyMagic, remaining());
        if (!sent.ok()) {
          return sent;
        }
        ready = true;
        return OperationResult::success();
      }
      return OperationResult::failure(ErrorCode::kTimeout, "等待数据端 hello 超时");
    }

    bool hello_sent = false;
    while (!hello_sent && remaining().count() > 0) {
      const auto attempt_budget = std::min(remaining(), std::chrono::milliseconds{50});
      const auto sent = send_single_frame(kHelloMagic, attempt_budget);
      if (sent.ok()) {
        hello_sent = true;
        break;
      }
      if (sent.error.code != ErrorCode::kTimeout) {
        return sent;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (!hello_sent) {
      return OperationResult::failure(ErrorCode::kTimeout, "发送数据端 hello 超时");
    }
    std::string frame;
    const auto ready_frame = receive_single_frame(frame, remaining());
    if (!ready_frame.ok()) {
      return ready_frame.error.code == ErrorCode::kTimeout
                 ? OperationResult::failure(ErrorCode::kTimeout, "等待数据端 ready 超时")
                 : ready_frame;
    }
    if (frame != kReadyMagic) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "就绪响应非法");
    }
    ready = true;
    return OperationResult::success();
  }
};

ZmqDataChannel::ZmqDataChannel(ZmqDataConfig config)
    : impl_(new Impl(std::move(config))) {
  if (!validate_zmq_data_config(impl_->config).ok()) {
    impl_->config = ZmqDataConfig{};
  }
}

ZmqDataChannel::~ZmqDataChannel() {
  if (impl_) {
    impl_->close_socket();
  }
}

domain::OperationResult ZmqDataChannel::bind(const std::string& endpoint) {
  if (!impl_) {
    return OperationResult::failure(ErrorCode::kBackendFailure, "数据通道未初始化");
  }
  if (impl_->socket) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted, "数据通道已经建立");
  }
  if (endpoint.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "绑定端点不能为空");
  }
  const auto socket_ready = impl_->make_socket();
  if (!socket_ready.ok()) {
    return socket_ready;
  }
  try {
    impl_->socket->bind(endpoint);
    impl_->bound_endpoint = impl_->socket->get(zmq::sockopt::last_endpoint);
    impl_->role = Impl::Role::kBinder;
    impl_->ready = false;
    impl_->input_sequence.reset();
    impl_->output_sequence.reset();
    return OperationResult::success();
  } catch (const zmq::error_t& error) {
    impl_->close_socket();
    return OperationResult::failure(ErrorCode::kBackendFailure, error.what());
  }
}

domain::OperationResult ZmqDataChannel::connect(const std::string& endpoint) {
  if (!impl_) {
    return OperationResult::failure(ErrorCode::kBackendFailure, "数据通道未初始化");
  }
  if (impl_->socket) {
    return OperationResult::failure(ErrorCode::kAlreadyCompleted, "数据通道已经建立");
  }
  if (endpoint.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "连接端点不能为空");
  }
  const auto socket_ready = impl_->make_socket();
  if (!socket_ready.ok()) {
    return socket_ready;
  }
  try {
    impl_->socket->connect(endpoint);
    impl_->bound_endpoint.clear();
    impl_->role = Impl::Role::kConnector;
    impl_->ready = false;
    impl_->input_sequence.reset();
    impl_->output_sequence.reset();
    return OperationResult::success();
  } catch (const zmq::error_t& error) {
    impl_->close_socket();
    return OperationResult::failure(ErrorCode::kBackendFailure, error.what());
  }
}

domain::OperationResult ZmqDataChannel::wait_ready(std::chrono::milliseconds timeout) {
  if (!impl_) {
    return OperationResult::failure(ErrorCode::kBackendFailure, "数据通道未初始化");
  }
  return impl_->wait_ready(timeout);
}

bool ZmqDataChannel::ready() const noexcept {
  return impl_ != nullptr && impl_->ready;
}

std::string ZmqDataChannel::bound_endpoint() const {
  if (!impl_) {
    return {};
  }
  return impl_->bound_endpoint;
}

domain::OperationResult ZmqDataChannel::send_input(const InputStreamEvent& event) {
  if (!impl_ || !impl_->ready) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "数据通道未就绪");
  }
  const auto metadata = encode_input_event_metadata(event);
  if (!metadata.ok()) {
    return OperationResult::failure(metadata.error.code, metadata.error.message);
  }
  const auto payload = encode_input_pcm_payload(event);
  if (!payload.ok()) {
    return OperationResult::failure(payload.error.code, payload.error.message);
  }
  if (metadata.value->size() > impl_->config.max_metadata_bytes ||
      payload.value->size() > impl_->config.max_payload_bytes) {
    ++impl_->counters.rejected;
    return OperationResult::failure(ErrorCode::kInvalidInput, "输入消息超过配置上限");
  }
  const auto sent = impl_->send_multipart(*metadata.value, *payload.value);
  if (sent.ok()) {
    ++impl_->counters.sent_input;
  } else {
    ++impl_->counters.send_failures;
    if (sent.error.code == ErrorCode::kTimeout) {
      ++impl_->counters.timeouts;
    }
  }
  return sent;
}

domain::OperationResult ZmqDataChannel::send_output(const DataEvent& event) {
  if (!impl_ || !impl_->ready) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "数据通道未就绪");
  }
  const auto validation = protocol::validate_event(event);
  if (!validation.ok()) {
    return OperationResult::failure(validation.error.code, validation.error.message);
  }
  const auto metadata = protocol::encode_event_metadata(event);
  if (!metadata.ok()) {
    return OperationResult::failure(metadata.error.code, metadata.error.message);
  }
  if (metadata.value->size() > impl_->config.max_metadata_bytes ||
      event.pcm.size() > impl_->config.max_payload_bytes) {
    ++impl_->counters.rejected;
    return OperationResult::failure(ErrorCode::kInvalidInput, "输出消息超过配置上限");
  }
  const auto sent = impl_->send_multipart(*metadata.value, event.pcm);
  if (sent.ok()) {
    ++impl_->counters.sent_output;
  } else {
    ++impl_->counters.send_failures;
    if (sent.error.code == ErrorCode::kTimeout) {
      ++impl_->counters.timeouts;
    }
  }
  return sent;
}

domain::Result<InputStreamEvent> ZmqDataChannel::receive_input(
    std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->ready) {
    return domain::Result<InputStreamEvent>::failure(ErrorCode::kInvalidInput,
                                                     "数据通道未就绪");
  }
  std::string metadata;
  std::vector<std::uint8_t> payload;
  const auto received = impl_->receive_multipart(metadata, payload, timeout);
  if (!received.ok()) {
    ++impl_->counters.rejected;
    if (received.error.code == ErrorCode::kTimeout) {
      ++impl_->counters.timeouts;
    }
    return domain::Result<InputStreamEvent>::failure(received.error.code,
                                                     received.error.message);
  }
  const auto decoded = decode_input_event_metadata(metadata);
  if (!decoded.ok()) {
    ++impl_->counters.rejected;
    return domain::Result<InputStreamEvent>::failure(decoded.error.code,
                                                     decoded.error.message);
  }
  const auto event = attach_input_pcm_payload(*decoded.value, payload);
  if (!event.ok()) {
    ++impl_->counters.rejected;
    return domain::Result<InputStreamEvent>::failure(event.error.code,
                                                     event.error.message);
  }
  const auto sequenced = impl_->input_sequence.accept(*event.value);
  if (!sequenced.ok()) {
    ++impl_->counters.rejected;
    return domain::Result<InputStreamEvent>::failure(sequenced.error.code,
                                                     sequenced.error.message);
  }
  ++impl_->counters.received_input;
  return event;
}

domain::Result<DataEvent> ZmqDataChannel::receive_output(
    std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->ready) {
    return domain::Result<DataEvent>::failure(ErrorCode::kInvalidInput,
                                              "数据通道未就绪");
  }
  std::string metadata;
  std::vector<std::uint8_t> payload;
  const auto received = impl_->receive_multipart(metadata, payload, timeout);
  if (!received.ok()) {
    ++impl_->counters.rejected;
    if (received.error.code == ErrorCode::kTimeout) {
      ++impl_->counters.timeouts;
    }
    return domain::Result<DataEvent>::failure(received.error.code,
                                              received.error.message);
  }
  const auto decoded = protocol::decode_event_metadata(metadata);
  if (!decoded.ok()) {
    ++impl_->counters.rejected;
    return domain::Result<DataEvent>::failure(decoded.error.code,
                                              decoded.error.message);
  }
  domain::Result<DataEvent> event = decoded;
  if (decoded.value->expected_pcm_bytes == 0) {
    if (!payload.empty()) {
      ++impl_->counters.rejected;
      return domain::Result<DataEvent>::failure(ErrorCode::kInvalidInput,
                                                "非 PCM 输出事件携带了载荷");
    }
  } else {
    event = protocol::attach_pcm_payload(*decoded.value, payload);
    if (!event.ok()) {
      ++impl_->counters.rejected;
      return domain::Result<DataEvent>::failure(event.error.code,
                                                event.error.message);
    }
  }
  const auto sequenced = impl_->output_sequence.accept(*event.value);
  if (!sequenced.ok()) {
    ++impl_->counters.rejected;
    return domain::Result<DataEvent>::failure(sequenced.error.code,
                                              sequenced.error.message);
  }
  ++impl_->counters.received_output;
  return event;
}

ZmqDataStats ZmqDataChannel::stats() const noexcept {
  if (!impl_) {
    return {};
  }
  return impl_->counters;
}

void ZmqDataChannel::close() noexcept {
  if (impl_) {
    impl_->close_socket();
  }
}

}  // namespace nexweave::transport

// 数据面脚本夹具：为跨进程取消与旧代际过滤提供可控的数据面对端。
//
// 它只用于测试，不进入产品安装目标。两种模式：
//   - block：完成 ready 握手后先阻塞一段时间不读输入，用来把代理输入队列压满；
//     恢复后消费 start/frame/end/cancel；收到 cancel 或 end 时回一条唯一终态。
//   - stale：主动发送一条正常轮次，然后发送同键旧终态和不同键旧终态，最后保持
//     存活等待父进程停止。它让测试能够验证“旧事件不能穿透到新请求”。
//
// 资源所有权：ZmqDataChannel 与 socket 由本进程创建并关闭；端点文件由父进程给出；
// 本对象不拥有音频、Session 或任何设备句柄。
#include "child_process.hpp"
#include "remote_session_proxy.hpp"
#include "zmq_data.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using nexweave::domain::ErrorCode;
using nexweave::protocol::DataEvent;
using nexweave::protocol::DataEventType;
using nexweave::transport::ZmqDataChannel;

std::string Env(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

int EnvInt(const char* name, int fallback) {
  const std::string value = Env(name);
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stoi(value);
  } catch (...) {
    return fallback;
  }
}

bool WriteEndpoint(const std::string& path, const std::string& endpoint) {
  std::ofstream file(path);
  if (!file.is_open()) {
    return false;
  }
  file << endpoint << '\n';
  return file.good();
}

bool WriteTrace(const std::string& path, const std::string& line) {
  if (path.empty()) {
    return true;
  }
  std::ofstream file(path, std::ios::app);
  if (!file.is_open()) {
    return false;
  }
  file << line << '\n';
  return file.good();
}

DataEvent MakeEvent(const std::string& request_id, const std::string& session_id,
                    std::uint64_t generation, std::uint64_t sequence,
                    DataEventType type) {
  DataEvent event;
  event.request_id = request_id;
  event.session_id = session_id;
  event.generation = generation;
  event.sequence = sequence;
  event.type = type;
  return event;
}

bool Send(ZmqDataChannel& channel, const DataEvent& event) {
  const auto validation = nexweave::protocol::validate_event(event);
  if (!validation.ok()) {
    std::cerr << "脚本夹具构造了非法事件: " << validation.error.message << std::endl;
    return false;
  }
  const auto sent = channel.send_output(event);
  if (!sent.ok()) {
    std::cerr << "脚本夹具发送失败: " << sent.error.message << std::endl;
    return false;
  }
  return true;
}

int RunStaleMode(ZmqDataChannel& channel) {
  const std::string session_id = "scripted-session";
  if (!Send(channel, MakeEvent("old-turn-1", session_id, 1, 1, DataEventType::kToken))) {
    return 8;
  }
  DataEvent done = MakeEvent("old-turn-1", session_id, 1, 2, DataEventType::kDone);
  done.end = true;
  if (!Send(channel, done)) {
    return 9;
  }
  DataEvent late_same_key =
      MakeEvent("old-turn-1", session_id, 1, 3, DataEventType::kToken);
  late_same_key.text = "late-same";
  if (!Send(channel, late_same_key)) {
    return 10;
  }
  DataEvent new_key = MakeEvent("old-turn-2", session_id, 1, 1, DataEventType::kToken);
  new_key.text = "late-new-key";
  if (!Send(channel, new_key)) {
    return 11;
  }
  DataEvent late_error = MakeEvent("old-turn-2", session_id, 1, 2, DataEventType::kError);
  late_error.end = true;
  late_error.error_code = ErrorCode::kBackendFailure;
  late_error.message = "迟到的旧错误";
  if (!Send(channel, late_error)) {
    return 12;
  }
  // 保持通道存活，让父进程在完成断言后通过停止路径回收本进程。
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return 0;
}

int RunBlockMode(ZmqDataChannel& channel) {
  const int delay_ms = EnvInt("NEXWEAVE_SCRIPTED_BLOCK_MS", 0);
  if (delay_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }
  const std::string trace_file = Env("NEXWEAVE_SCRIPTED_TRACE_FILE");
  std::string stream_id = "scripted-session";
  while (true) {
    const auto received = channel.receive_input(std::chrono::milliseconds(100));
    if (!received.ok()) {
      if (received.error.code == ErrorCode::kTimeout) {
        continue;
      }
      return 0;
    }
    const auto& event = *received.value;
    if (event.kind == nexweave::runtime::InputEventKind::kStart) {
      stream_id = event.stream_id;
      continue;
    }
    if (event.kind == nexweave::runtime::InputEventKind::kFrame) {
      continue;
    }
    if (event.kind == nexweave::runtime::InputEventKind::kCancel) {
      (void)WriteTrace(trace_file, "cancel");
      DataEvent terminal =
          MakeEvent("scripted-cancel-turn", stream_id, event.generation, 1, DataEventType::kError);
      terminal.end = true;
      terminal.error_code = ErrorCode::kCancelled;
      terminal.message = "脚本夹具已取消";
      (void)Send(channel, terminal);
      // linger=0 的测试通道在进程退出时会丢弃尚未被对端取走的排队的消息；短暂等待
      // 父进程 I/O 线程消费终态，避免把“测试夹具正常退出”误判成“取消事件丢失”。
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      return 0;
    }
    if (event.kind == nexweave::runtime::InputEventKind::kEnd) {
      (void)WriteTrace(trace_file, "end");
      DataEvent terminal =
          MakeEvent("scripted-end-turn", stream_id, event.generation, 1, DataEventType::kDone);
      terminal.end = true;
      (void)Send(channel, terminal);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      return 0;
    }
  }
}

}  // namespace

int main() {
  const std::string endpoint_file =
      Env(nexweave::transport::kRemoteSessionEndpointFileEnvironment);
  if (endpoint_file.empty()) {
    std::cerr << "缺少远端数据面端点文件环境变量" << std::endl;
    return 2;
  }

  ZmqDataChannel channel;
  const auto bound = channel.bind("tcp://127.0.0.1:*");
  if (!bound.ok()) {
    std::cerr << "脚本夹具绑定失败: " << bound.error.message << std::endl;
    return 3;
  }
  if (!WriteEndpoint(endpoint_file, channel.bound_endpoint())) {
    std::cerr << "脚本夹具写入端点失败" << std::endl;
    return 4;
  }
  const auto notified = nexweave::runtime::notify_parent_ready();
  if (!notified.ok()) {
    std::cerr << "脚本夹具就绪通知失败: " << notified.error.message << std::endl;
    return 5;
  }
  const auto ready = channel.wait_ready(std::chrono::milliseconds(5000));
  if (!ready.ok()) {
    std::cerr << "脚本夹具等待数据面 ready 失败: " << ready.error.message << std::endl;
    return 6;
  }

  const std::string mode = Env("NEXWEAVE_SCRIPTED_MODE");
  if (mode == "stale") {
    return RunStaleMode(channel);
  }
  return RunBlockMode(channel);
}

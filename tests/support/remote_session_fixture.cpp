// 远端会话集成测试夹具：在独立进程中运行真实的 SessionApp 与数据面服务端。
//
// 它读取代理写入的 NEXWEAVE_REMOTE_SESSION_ENDPOINT_FILE 环境变量，绑定数据面端点、
// 写出实际端点，然后调用 notify_parent_ready()。父进程只有在收到就绪通知后才连接，
// 因此“进程已启动”不会和“业务已就绪”混为一谈。
#include "child_process.hpp"
#include "fake_asr.hpp"
#include "fake_audio.hpp"
#include "fake_llm.hpp"
#include "fake_rag.hpp"
#include "fake_tts.hpp"
#include "queued_audio_source.hpp"
#include "remote_session_proxy.hpp"
#include "remote_session_server.hpp"
#include "session_app.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void HandleSignal(int) {
  g_stop_requested = 1;
}

class AmplitudeDetector final : public nexweave::runtime::ISpeechActivityDetector {
 public:
  nexweave::domain::Result<nexweave::runtime::SpeechActivity> detect(
      const nexweave::domain::AudioFrame& frame) override {
    for (const std::int16_t sample : frame.samples) {
      if (sample != 0) {
        return nexweave::domain::Result<nexweave::runtime::SpeechActivity>::success(
            nexweave::runtime::SpeechActivity::kSpeech);
      }
    }
    return nexweave::domain::Result<nexweave::runtime::SpeechActivity>::success(
        nexweave::runtime::SpeechActivity::kSilence);
  }

  void reset() override {}
};

class TickingClock final : public nexweave::runtime::IPlaybackClock {
 public:
  std::int64_t now_ms() const noexcept override {
    const_cast<TickingClock*>(this)->now_ms_ +=
        static_cast<std::int64_t>(nexweave::domain::kAudioFrameDurationMs);
    return now_ms_;
  }

  void advance(std::int64_t delta_ms) override {
    if (delta_ms > 0) {
      now_ms_ += delta_ms;
    }
  }

 private:
  std::int64_t now_ms_ = 0;
};

std::string EndpointFilePath() {
  const char* value = std::getenv(nexweave::transport::kRemoteSessionEndpointFileEnvironment);
  return value == nullptr ? std::string() : std::string(value);
}

std::string TraceFilePath() {
  const char* value = std::getenv("NEXWEAVE_REMOTE_SESSION_TRACE_FILE");
  return value == nullptr ? std::string() : std::string(value);
}

void WriteTrace(const std::string& path, const std::vector<nexweave::runtime::ActivityMarker>& trace) {
  if (path.empty()) {
    return;
  }
  std::ofstream file(path);
  if (!file.is_open()) {
    return;
  }
  for (const auto marker : trace) {
    file << static_cast<int>(marker) << "\n";
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

}  // namespace

int main() {
  const std::string endpoint_file = EndpointFilePath();
  if (endpoint_file.empty()) {
    std::cerr << "缺少远端会话端点文件环境变量" << std::endl;
    return 2;
  }
  const std::string trace_file = TraceFilePath();

  std::signal(SIGTERM, HandleSignal);
  std::signal(SIGINT, HandleSignal);

  using nexweave::backend::FakeAsr;
  using nexweave::backend::FakeAudioSink;
  using nexweave::backend::FakeLlm;
  using nexweave::backend::FakeRag;
  using nexweave::backend::FakeRagRouter;
  using nexweave::backend::FakeTts;
  using nexweave::capability::RetrievedChunk;
  using nexweave::runtime::LogicalClockPlayback;
  using nexweave::runtime::QueuedAudioSource;
  using nexweave::runtime::QueuedAudioSourceConfig;
  using nexweave::runtime::ResidentAudioInput;
  using nexweave::runtime::ResidentAudioInputConfig;
  using nexweave::runtime::SessionApp;
  using nexweave::runtime::SessionAppConfig;
  using nexweave::runtime::SessionAppInputMode;
  using nexweave::transport::RemoteSessionServer;
  using nexweave::transport::RemoteSessionServerConfig;

  FakeAsr asr({"远端会话回答"});
  FakeRag retriever(std::vector<RetrievedChunk>{});
  FakeRagRouter router(retriever);
  FakeTts tts;
  FakeLlm llm({"远端", "会话", "回答"});
  TickingClock clock;
  FakeAudioSink sink;
  if (!sink.open().ok()) {
    std::cerr << "音频汇打开失败" << std::endl;
    return 3;
  }
  LogicalClockPlayback playback(sink, clock);

  QueuedAudioSourceConfig source_config;
  source_config.max_pending_frames = 64;
  QueuedAudioSource source(source_config);

  ResidentAudioInputConfig resident_config;
  resident_config.segmentation.preroll_frames = 0;
  resident_config.segmentation.min_speech_frames = 1;
  resident_config.segmentation.min_silence_frames = 2;
  resident_config.segmentation.max_speech_frames = 1000;
  AmplitudeDetector detector;
  ResidentAudioInput resident(source, detector, "remote-session", resident_config);

  SessionAppConfig app_config;
  app_config.mode = SessionAppInputMode::kSimulatedResident;
  app_config.stream_id = "remote-session";
  app_config.resident_config = resident_config;
  app_config.session_config.playback_queue_capacity = 256;
  app_config.pump_budget_per_take = 4096;
  SessionApp app(app_config, asr, retriever, router, tts, playback, &resident, &llm);

  RemoteSessionServerConfig server_config;
  server_config.endpoint = "tcp://127.0.0.1:*";
  server_config.output_queue_capacity = 256;
  RemoteSessionServer server(app, source, server_config);

  const auto bound = server.bind();
  if (!bound.ok()) {
    std::cerr << "绑定数据面失败: " << bound.error.message << std::endl;
    return 4;
  }
  if (!WriteEndpoint(endpoint_file, server.bound_endpoint())) {
    std::cerr << "写入端点文件失败" << std::endl;
    return 5;
  }
  const auto notified = nexweave::runtime::notify_parent_ready();
  if (!notified.ok()) {
    std::cerr << "就绪通知失败: " << notified.error.message << std::endl;
    return 6;
  }

  std::atomic<bool> monitor_stop{false};
  std::thread monitor([&server, &monitor_stop] {
    while (!monitor_stop.load()) {
      if (g_stop_requested != 0) {
        server.request_stop();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  const auto served = server.serve();
  monitor_stop.store(true);
  if (monitor.joinable()) {
    monitor.join();
  }
  WriteTrace(trace_file, app.trace());
  if (!served.ok() && served.error.code != nexweave::domain::ErrorCode::kCancelled) {
    std::cerr << "远端会话失败: " << served.error.message << std::endl;
    return 7;
  }
  // 测试夹具模拟由父进程拥有的常驻子进程：serve() 返回后不主动退出，保留数据面 socket，
  // 直到父进程发送停止信号。刚交付的终态因此不会被进程退出与父进程读取之间的竞争丢弃；
  // 真实部署也应由代理/监督器的停止顺序决定子进程何时退出。
  while (g_stop_requested == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return 0;
}

// 全双工音频前端行为测试。
//
// 测试只通过 capability::IAudioSource/IAudioSink、受控 PCM 后端和受控前处理器
// 观察外部行为，不访问真实声卡、WebRTC 或文件。保护的不变量：
//   - 20 ms 领域帧始终拆成两个 10 ms 窗口，前处理器不会收到 320 样本帧；
//   - 停止回答/播放不会关闭常驻采集；
//   - 实际成功写入设备的播放样本会转换为 16 kHz 参考并进入 AEC 时间轴；
//   - 只有近端说话时参考为零，不会伪造回声参考；
//   - 播放参考不连续会重置前处理器状态；
//   - 取消能在有限等待内唤醒阻塞读，close/open 后可重新开始；
//   - 采集设备失效后按配置预算有限重开，成功后转换与 AEC 状态复位；
//   - 前处理失败以结构化错误收敛，不会静默透传；
//   - 重复启停与析构都会释放线程、设备和前处理器。
#include "audio_frontend.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../support/fake_audio_processor.hpp"
#include "../support/fake_pcm_capture_backend.hpp"
#include "../support/fake_pcm_playback_backend.hpp"

namespace {

using nexweave::backend::AlsaAudioSinkConfig;
using nexweave::backend::AlsaAudioSourceConfig;
using nexweave::backend::FakePcmCaptureBackend;
using nexweave::backend::FakePcmPlaybackBackend;
using nexweave::backend::PcmCaptureBackendConfig;
using nexweave::backend::PcmCaptureFormat;
using nexweave::backend::PcmCaptureReadKind;
using nexweave::backend::PcmPlaybackBackendConfig;
using nexweave::backend::PcmPlaybackFormat;
using nexweave::backend::PcmSampleFormat;
using nexweave::backend::PlaybackSampleFormat;
using nexweave::domain::AudioFrame;
using nexweave::domain::ErrorCode;
using nexweave::runtime::AudioFrontendConfig;
using nexweave::runtime::AudioFrontendStats;
using nexweave::runtime::AudioPipelineState;
using nexweave::runtime::AudioProcessorState;
using nexweave::runtime::DuplexAudioFrontend;
using nexweave::runtime::FakeAudioProcessor;

int failures = 0;

#define CHECK(value)                                               \
  do {                                                             \
    if (!(value)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
      ++failures;                                                  \
    }                                                              \
  } while (false)

PcmCaptureBackendConfig MakeCaptureBackendConfig() {
  PcmCaptureBackendConfig config;
  config.device = "fake-capture";
  config.sample_rate_hz = 16000;
  config.channels = 1;
  config.format = PcmSampleFormat::kS16LE;
  config.period_frames = 320;
  config.buffer_frames = 1280;
  config.read_poll_slice_ms = 10;
  return config;
}

PcmPlaybackBackendConfig MakePlaybackBackendConfig() {
  PcmPlaybackBackendConfig config;
  config.device = "fake-playback";
  config.sample_rate_hz = 16000;
  config.channels = 1;
  config.format = PlaybackSampleFormat::kS16LE;
  config.period_frames = 320;
  config.buffer_frames = 1280;
  config.write_poll_slice_ms = 10;
  config.drain_poll_slice_ms = 10;
  return config;
}

PcmPlaybackBackendConfig MakePlaybackBackendConfig44100Stereo() {
  PcmPlaybackBackendConfig config;
  config.device = "fake-playback-44k-stereo";
  config.sample_rate_hz = 44100;
  config.channels = 2;
  config.format = PlaybackSampleFormat::kS16LE;
  config.period_frames = 441;
  config.buffer_frames = 1764;
  config.write_poll_slice_ms = 10;
  config.drain_poll_slice_ms = 10;
  return config;
}

AlsaAudioSourceConfig MakeSourceConfig(std::size_t reconnect_budget = 0) {
  AlsaAudioSourceConfig config;
  config.read_timeout = std::chrono::milliseconds(200);
  config.max_backend_reads_per_frame = 64;
  config.reconnect_budget = reconnect_budget;
  config.max_recoveries_per_frame = 8;
  return config;
}

AlsaAudioSinkConfig MakeSinkConfig() {
  AlsaAudioSinkConfig config;
  config.write_timeout = std::chrono::milliseconds(200);
  config.max_backend_writes_per_frame = 64;
  config.reconnect_budget = 0;
  config.max_recoveries_per_frame = 8;
  config.drain_timeout = std::chrono::milliseconds(200);
  return config;
}

AudioFrontendConfig MakeFrontendConfig(std::size_t reconnect_budget = 0) {
  AudioFrontendConfig config;
  config.source = MakeSourceConfig(reconnect_budget);
  config.sink = MakeSinkConfig();
  config.processed_frame_queue_capacity = 8;
  config.render_reference_sample_capacity = 32000;
  config.reference_schedule_tolerance = std::chrono::milliseconds(5);
  config.max_reference_lateness = std::chrono::milliseconds(50);
  config.reconnect_retry_interval = std::chrono::milliseconds(20);
  config.reconnect_total_budget = std::chrono::milliseconds(200);
  return config;
}

PcmCaptureFormat CaptureFormat() {
  PcmCaptureFormat format;
  format.sample_rate_hz = 16000;
  format.channels = 1;
  format.format = PcmSampleFormat::kS16LE;
  return format;
}

AudioFrame MakeFrame(std::int16_t value) {
  const auto made =
      AudioFrame::from_samples(std::vector<std::int16_t>(320, value));
  CHECK(made.ok());
  return made.value;
}

std::vector<std::int32_t> MakeNative(std::int32_t value) {
  return std::vector<std::int32_t>(320, value);
}

void CloseFrontend(DuplexAudioFrontend& frontend) {
  (void)frontend.CloseCapture();
  (void)frontend.ClosePlayback();
}

void TestProcessesTenMillisecondWindows() {
  auto backend = std::make_unique<FakePcmCaptureBackend>(MakeCaptureBackendConfig());
  FakePcmCaptureBackend* backend_ptr = backend.get();
  backend_ptr->set_block_on_read(true);
  backend_ptr->push_data(CaptureFormat(), MakeNative(1000));

  auto processor = std::make_unique<FakeAudioProcessor>(7);
  FakeAudioProcessor* processor_ptr = processor.get();

  DuplexAudioFrontend frontend(MakeFrontendConfig(), std::move(backend),
                               std::make_unique<FakePcmPlaybackBackend>(
                                   MakePlaybackBackendConfig()),
                               std::move(processor));
  CHECK(frontend.source().open().ok());
  const auto read = frontend.source().read();
  CHECK(read.ok());
  if (read.ok()) {
    CHECK(read.value->samples.size() == 320);
    for (std::int16_t sample : read.value->samples) {
      CHECK(sample == 7);
    }
  }
  const auto render_frames = processor_ptr->render_frames();
  const auto capture_frames = processor_ptr->capture_frames();
  CHECK(render_frames.size() == 2);
  CHECK(capture_frames.size() == 2);
  for (const auto& frame : render_frames) {
    CHECK(frame.size() == 160);
  }
  for (const auto& frame : capture_frames) {
    CHECK(frame.size() == 160);
    CHECK(frame[0] == 1000);
    CHECK(frame[159] == 1000);
  }
  CloseFrontend(frontend);
  CHECK(backend_ptr->stats().close_calls >= 1);
}

void TestOnlyNearEndUsesZeroReference() {
  auto backend = std::make_unique<FakePcmCaptureBackend>(MakeCaptureBackendConfig());
  FakePcmCaptureBackend* backend_ptr = backend.get();
  backend_ptr->set_block_on_read(true);
  backend_ptr->push_data(CaptureFormat(), MakeNative(2000));

  auto processor = std::make_unique<FakeAudioProcessor>(9);
  FakeAudioProcessor* processor_ptr = processor.get();
  DuplexAudioFrontend frontend(MakeFrontendConfig(), std::move(backend),
                               std::make_unique<FakePcmPlaybackBackend>(
                                   MakePlaybackBackendConfig()),
                               std::move(processor));
  CHECK(frontend.source().open().ok());
  const auto read = frontend.source().read();
  CHECK(read.ok());
  const auto render_frames = processor_ptr->render_frames();
  CHECK(render_frames.size() == 2);
  for (const auto& frame : render_frames) {
    for (std::int16_t sample : frame) {
      CHECK(sample == 0);
    }
  }
  const auto stats = frontend.stats();
  CHECK(stats.render_reference_zero_filled == 320);
  CloseFrontend(frontend);
}

void TestPlaybackReferenceUsesActualWrittenSamples() {
  auto capture = std::make_unique<FakePcmCaptureBackend>(MakeCaptureBackendConfig());
  FakePcmCaptureBackend* capture_ptr = capture.get();
  capture_ptr->set_block_on_read(true);

  auto playback = std::make_unique<FakePcmPlaybackBackend>(
      MakePlaybackBackendConfig());
  FakePcmPlaybackBackend* playback_ptr = playback.get();
  playback_ptr->set_auto_accept(true);

  auto processor = std::make_unique<FakeAudioProcessor>(11);
  FakeAudioProcessor* processor_ptr = processor.get();
  DuplexAudioFrontend frontend(MakeFrontendConfig(), std::move(capture),
                               std::move(playback), std::move(processor));
  CHECK(frontend.source().open().ok());
  CHECK(frontend.sink().open().ok());

  const auto written = frontend.sink().write(MakeFrame(20000));
  CHECK(written.ok());
  // 等待两个 10 ms 捕获槽都越过参考写入时间，避免把“参考尚未到播放时间”
  // 误判成前端没有把实际写入样本接入 AEC。
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  capture_ptr->push_data(CaptureFormat(), MakeNative(3000));

  const auto read = frontend.source().read();
  CHECK(read.ok());
  const auto render_frames = processor_ptr->render_frames();
  CHECK(!render_frames.empty());
  for (const auto& frame : render_frames) {
    CHECK(frame.size() == 160);
    CHECK(frame[0] == 20000);
    CHECK(frame[159] == 20000);
  }
  CHECK(playback_ptr->accepted_samples().size() == 320);
  CHECK(frontend.stats().render_reference_blocks >= 1);
  CloseFrontend(frontend);
}

void TestPlaybackReferenceResamples44kStereo() {
  auto capture = std::make_unique<FakePcmCaptureBackend>(MakeCaptureBackendConfig());
  FakePcmCaptureBackend* capture_ptr = capture.get();
  capture_ptr->set_block_on_read(true);

  auto playback = std::make_unique<FakePcmPlaybackBackend>(
      MakePlaybackBackendConfig44100Stereo());
  FakePcmPlaybackBackend* playback_ptr = playback.get();
  playback_ptr->set_auto_accept(true);
  auto processor = std::make_unique<FakeAudioProcessor>(0);
  FakeAudioProcessor* processor_ptr = processor.get();
  DuplexAudioFrontend frontend(MakeFrontendConfig(), std::move(capture),
                               std::move(playback), std::move(processor));
  CHECK(frontend.source().open().ok());
  CHECK(frontend.sink().open().ok());
  CHECK(frontend.sink().write(MakeFrame(20000)).ok());
  // 睡眠 15 ms 足以让两个 10 ms 参考槽进入播放时间；睡眠超过配置的 50 ms
  // 迟到上界会把 AEC 重置当成“测试自己造成的迟到”，反而掩盖重采样路径。
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  capture_ptr->push_data(CaptureFormat(), MakeNative(2500));
  const auto read = frontend.source().read();
  CHECK(read.ok());
  const auto render_frames = processor_ptr->render_frames();
  CHECK(render_frames.size() == 2);
  for (const auto& frame : render_frames) {
    CHECK(frame.size() == 160);
    for (std::int16_t sample : frame) {
      CHECK(sample == 20000);
    }
  }
  CHECK(playback_ptr->accepted_samples().size() >= 800);
  CHECK(frontend.stats().render_reference_blocks >= 1);
  CloseFrontend(frontend);
}

void TestPlaybackCancelResetsProcessorAlignment() {
  auto capture = std::make_unique<FakePcmCaptureBackend>(MakeCaptureBackendConfig());
  FakePcmCaptureBackend* capture_ptr = capture.get();
  capture_ptr->set_block_on_read(true);

  auto playback = std::make_unique<FakePcmPlaybackBackend>(
      MakePlaybackBackendConfig());
  playback->set_auto_accept(true);
  auto processor = std::make_unique<FakeAudioProcessor>(13);
  FakeAudioProcessor* processor_ptr = processor.get();
  DuplexAudioFrontend frontend(MakeFrontendConfig(), std::move(capture),
                               std::move(playback), std::move(processor));
  CHECK(frontend.source().open().ok());
  CHECK(frontend.sink().open().ok());
  CHECK(frontend.sink().cancel().ok());
  capture_ptr->push_data(CaptureFormat(), MakeNative(4000));
  const auto read = frontend.source().read();
  CHECK(read.ok());
  CHECK(processor_ptr->reset_calls() >= 1);
  CHECK(frontend.stats().render_reference_discontinuities >= 1);
  CloseFrontend(frontend);
}

void TestCancelWakesBlockedReadAndReopenWorks() {
  auto backend = std::make_unique<FakePcmCaptureBackend>(MakeCaptureBackendConfig());
  FakePcmCaptureBackend* backend_ptr = backend.get();
  backend_ptr->set_block_on_read(true);
  auto processor = std::make_unique<FakeAudioProcessor>(17);
  DuplexAudioFrontend frontend(MakeFrontendConfig(), std::move(backend),
                               std::make_unique<FakePcmPlaybackBackend>(
                                   MakePlaybackBackendConfig()),
                               std::move(processor));
  CHECK(frontend.source().open().ok());

  nexweave::domain::Result<AudioFrame> read_result;
  std::thread reader([&]() { read_result = frontend.source().read(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(frontend.source().cancel().ok());
  reader.join();
  CHECK(!read_result.ok());
  CHECK(read_result.error.code == ErrorCode::kCancelled);

  CHECK(frontend.source().close().ok());
  backend_ptr->push_data(CaptureFormat(), MakeNative(5000));
  CHECK(frontend.source().open().ok());
  const auto resumed = frontend.source().read();
  CHECK(resumed.ok());
  if (resumed.ok()) {
    CHECK(resumed.value->samples.size() == 320);
  }
  CloseFrontend(frontend);
  CHECK(backend_ptr->stats().cancel_calls >= 1);
}

void TestCaptureDeviceReconnectUsesConfiguredBudget() {
  auto backend = std::make_unique<FakePcmCaptureBackend>(MakeCaptureBackendConfig());
  FakePcmCaptureBackend* backend_ptr = backend.get();
  backend_ptr->set_block_on_read(true);
  auto processor = std::make_unique<FakeAudioProcessor>(19);
  FakeAudioProcessor* processor_ptr = processor.get();
  DuplexAudioFrontend frontend(MakeFrontendConfig(), std::move(backend),
                               std::make_unique<FakePcmPlaybackBackend>(
                                   MakePlaybackBackendConfig()),
                               std::move(processor));
  CHECK(frontend.source().open().ok());

  backend_ptr->push_open_result(
      nexweave::domain::OperationResult::failure(ErrorCode::kDeviceFailure,
                                                 "第一次重开失败"));
  backend_ptr->push_open_result(nexweave::domain::OperationResult::success());
  backend_ptr->push_status(PcmCaptureReadKind::kDeviceFailure,
                           ErrorCode::kDeviceFailure, "模拟设备断开");

  nexweave::domain::Result<AudioFrame> read_result;
  std::thread reader([&]() { read_result = frontend.source().read(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  backend_ptr->push_data(CaptureFormat(), MakeNative(6000));
  reader.join();
  CHECK(read_result.ok());
  if (read_result.ok()) {
    CHECK(read_result.value->samples.size() == 320);
  }
  const auto stats = frontend.stats();
  CHECK(stats.capture_reconnect_attempts >= 1);
  CHECK(stats.capture_reconnect_successes >= 1);
  CHECK(stats.capture_reconnect_failures == 0);
  CHECK(processor_ptr->reset_calls() >= 1);
  CloseFrontend(frontend);
}

void TestProcessorFailureIsStructured() {
  auto backend = std::make_unique<FakePcmCaptureBackend>(MakeCaptureBackendConfig());
  FakePcmCaptureBackend* backend_ptr = backend.get();
  backend_ptr->set_block_on_read(true);
  backend_ptr->push_data(CaptureFormat(), MakeNative(7000));
  auto processor = std::make_unique<FakeAudioProcessor>(21);
  processor->set_capture_error(ErrorCode::kBackendFailure, "模拟前处理失败");
  DuplexAudioFrontend frontend(MakeFrontendConfig(), std::move(backend),
                               std::make_unique<FakePcmPlaybackBackend>(
                                   MakePlaybackBackendConfig()),
                               std::move(processor));
  CHECK(frontend.source().open().ok());
  const auto read = frontend.source().read();
  CHECK(!read.ok());
  CHECK(read.error.code == ErrorCode::kBackendFailure);
  CHECK(frontend.capture_state() == AudioPipelineState::kFailed);
  CHECK(!frontend.processor_error().ok());
  CloseFrontend(frontend);
}

void TestRepeatedStartStopAndDestructorCleanup() {
  auto capture = std::make_unique<FakePcmCaptureBackend>(MakeCaptureBackendConfig());
  FakePcmCaptureBackend* capture_ptr = capture.get();
  capture_ptr->set_block_on_read(true);

  auto playback = std::make_unique<FakePcmPlaybackBackend>(
      MakePlaybackBackendConfig());
  FakePcmPlaybackBackend* playback_ptr = playback.get();
  playback_ptr->set_auto_accept(true);
  auto processor = std::make_unique<FakeAudioProcessor>(23);
  DuplexAudioFrontend frontend(MakeFrontendConfig(), std::move(capture),
                               std::move(playback), std::move(processor));

  for (int round = 0; round < 5; ++round) {
    CHECK(frontend.source().open().ok());
    CHECK(frontend.sink().open().ok());
    CHECK(frontend.sink().write(MakeFrame(100)).ok());
    CHECK(frontend.sink().close().ok());
    CHECK(frontend.source().close().ok());
  }
  CHECK(capture_ptr->stats().open_successes == 5);
  CHECK(capture_ptr->stats().close_calls == 5);
  CHECK(playback_ptr->stats().open_successes == 5);
  CHECK(playback_ptr->stats().close_calls == 5);
}

}  // namespace

int main() {
  TestProcessesTenMillisecondWindows();
  TestOnlyNearEndUsesZeroReference();
  TestPlaybackReferenceUsesActualWrittenSamples();
  TestPlaybackReferenceResamples44kStereo();
  TestPlaybackCancelResetsProcessorAlignment();
  TestCancelWakesBlockedReadAndReopenWorks();
  TestCaptureDeviceReconnectUsesConfiguredBudget();
  TestProcessorFailureIsStructured();
  TestRepeatedStartStopAndDestructorCleanup();

  if (failures != 0) {
    std::printf("audio_frontend: %d check(s) failed\n", failures);
    return 1;
  }
  std::printf("audio_frontend: all checks passed\n");
  return 0;
}

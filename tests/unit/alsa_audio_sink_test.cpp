// ALSA 音频输出适配器行为测试。
//
// 测试只通过 capability::IAudioSink、受控 PCM 播放后端和回放参考回调观察外部行为，
// 不访问真实声卡。保护的不变量：
//   - 16 kHz/单声道/S16_LE/320 帧被显式转换为设备原生采样率/声道/编码；
//   - 短写、underrun、超时、取消、断开和重连都不会空转或丢失已接受数据顺序；
//   - write 成功只表示设备接受数据，正常 close 区分排空与取消丢弃；
//   - 回放参考来自实际成功写入的原生样本，包含顺序、时间、延迟和连续标志；
//   - 取消能唤醒阻塞写；close/open 后才能开始下一轮，重复 open 不清除取消状态。
#include "alsa/alsa_audio_sink.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "../support/fake_pcm_playback_backend.hpp"

namespace {

using nexweave::backend::AlsaAudioSink;
using nexweave::backend::AlsaAudioSinkConfig;
using nexweave::backend::AlsaPlaybackReferenceBlock;
using nexweave::backend::FakePcmPlaybackBackend;
using nexweave::backend::PcmPlaybackBackendConfig;
using nexweave::backend::PcmPlaybackFormat;
using nexweave::backend::PcmPlaybackWriteKind;
using nexweave::backend::PcmPlaybackWriteResult;
using nexweave::backend::PlaybackSampleFormat;
using nexweave::domain::AudioFrame;
using nexweave::domain::ErrorCode;

int failures = 0;

#define CHECK(value)                                               \
  do {                                                             \
    if (!(value)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
      ++failures;                                                  \
    }                                                              \
  } while (false)

PcmPlaybackBackendConfig MakeBackendConfig(
    std::uint32_t rate = 16000, std::uint16_t channels = 1,
    PlaybackSampleFormat format = PlaybackSampleFormat::kS16LE) {
  PcmPlaybackBackendConfig config;
  config.device = "fake:0";
  config.sample_rate_hz = rate;
  config.channels = channels;
  config.format = format;
  config.period_frames = 320;
  config.buffer_frames = 1280;
  config.write_poll_slice_ms = 10;
  config.drain_poll_slice_ms = 5;
  return config;
}

AlsaAudioSinkConfig MakeSinkConfig(std::size_t timeout_ms = 300,
                                   std::size_t max_writes = 32,
                                   std::size_t reconnect_budget = 2,
                                   std::size_t max_recoveries = 4,
                                   std::size_t drain_timeout_ms = 1000) {
  AlsaAudioSinkConfig config;
  config.write_timeout = std::chrono::milliseconds(timeout_ms);
  config.max_backend_writes_per_frame = max_writes;
  config.reconnect_budget = reconnect_budget;
  config.max_recoveries_per_frame = max_recoveries;
  config.drain_timeout = std::chrono::milliseconds(drain_timeout_ms);
  return config;
}

std::vector<std::int16_t> Ramp(std::size_t count, std::int16_t start) {
  std::vector<std::int16_t> samples(count);
  for (std::size_t index = 0; index < count; ++index) {
    samples[index] = static_cast<std::int16_t>(start + static_cast<std::int16_t>(index));
  }
  return samples;
}

std::vector<std::int16_t> Constant(std::size_t count, std::int16_t value) {
  return std::vector<std::int16_t>(count, value);
}

AudioFrame MakeFrame(const std::vector<std::int16_t>& samples) {
  const auto frame = AudioFrame::from_samples(samples);
  if (!frame.ok()) {
    std::printf("FAIL 无法构造测试帧\n");
    ++failures;
    return AudioFrame{};
  }
  return frame.value;
}

class SinkFixture {
 public:
  explicit SinkFixture(PcmPlaybackBackendConfig backend_config = MakeBackendConfig(),
                       AlsaAudioSinkConfig sink_config = MakeSinkConfig()) {
    auto backend = std::make_unique<FakePcmPlaybackBackend>(std::move(backend_config));
    backend_ = backend.get();
    sink_ = std::make_unique<AlsaAudioSink>(std::move(sink_config), std::move(backend));
  }

  FakePcmPlaybackBackend& backend() { return *backend_; }
  AlsaAudioSink& sink() { return *sink_; }

 private:
  FakePcmPlaybackBackend* backend_ = nullptr;
  std::unique_ptr<AlsaAudioSink> sink_;
};

struct ReferenceCollector {
  std::vector<AlsaPlaybackReferenceBlock> blocks;

  void Collect(const AlsaPlaybackReferenceBlock& block) {
    blocks.push_back(block);
  }
};

void TestConfigValidation() {
  PcmPlaybackBackendConfig backend = MakeBackendConfig();
  backend.device.clear();
  CHECK(!nexweave::backend::validate_pcm_playback_backend_config(backend).ok());

  backend = MakeBackendConfig();
  backend.channels = 3;
  CHECK(!nexweave::backend::validate_pcm_playback_backend_config(backend).ok());

  backend = MakeBackendConfig();
  backend.period_frames = 0;
  CHECK(!nexweave::backend::validate_pcm_playback_backend_config(backend).ok());

  backend = MakeBackendConfig();
  backend.buffer_frames = backend.period_frames - 1;
  CHECK(!nexweave::backend::validate_pcm_playback_backend_config(backend).ok());

  backend = MakeBackendConfig();
  backend.write_poll_slice_ms = 0;
  CHECK(!nexweave::backend::validate_pcm_playback_backend_config(backend).ok());

  CHECK(nexweave::backend::validate_pcm_playback_backend_config(
            MakeBackendConfig())
            .ok());

  AlsaAudioSinkConfig sink = MakeSinkConfig();
  sink.write_timeout = std::chrono::milliseconds(0);
  CHECK(!nexweave::backend::validate_alsa_audio_sink_config(sink).ok());

  sink = MakeSinkConfig();
  sink.max_backend_writes_per_frame = 0;
  CHECK(!nexweave::backend::validate_alsa_audio_sink_config(sink).ok());

  sink = MakeSinkConfig();
  sink.drain_timeout = std::chrono::milliseconds(0);
  CHECK(!nexweave::backend::validate_alsa_audio_sink_config(sink).ok());

  sink = MakeSinkConfig(300, 32, 0, 0);
  CHECK(nexweave::backend::validate_alsa_audio_sink_config(sink).ok());
}

void TestOpenFailureAndDiagnostics() {
  auto backend = std::make_unique<FakePcmPlaybackBackend>(MakeBackendConfig());
  backend->set_open_result(nexweave::domain::OperationResult::failure(
      ErrorCode::kDeviceFailure, "fake 播放设备缺失"));
  AlsaAudioSink sink(MakeSinkConfig(), std::move(backend));
  const auto opened = sink.open();
  CHECK(!opened.ok());
  CHECK(opened.error.code == ErrorCode::kDeviceFailure);
  const auto frame = MakeFrame(Constant(320, 1));
  CHECK(sink.write(frame).error.code == ErrorCode::kDeviceFailure);
  CHECK(sink.close().ok());
  CHECK(sink.close().ok());

  AlsaAudioSink without_backend(MakeSinkConfig(), nullptr);
  CHECK(without_backend.open().error.code == ErrorCode::kInvalidInput);
}

void TestFixedFramePassThroughAndReference() {
  SinkFixture fixture;
  auto& sink = fixture.sink();
  auto& backend = fixture.backend();
  ReferenceCollector collector;
  sink.set_playback_reference_callback(
      [&collector](const AlsaPlaybackReferenceBlock& block) { collector.Collect(block); });

  CHECK(sink.open().ok());
  CHECK(sink.open().error.code == ErrorCode::kAlreadyCompleted);
  const auto samples = Ramp(320, 100);
  CHECK(sink.write(MakeFrame(samples)).ok());
  CHECK(backend.accepted_samples().size() == 320);
  CHECK(backend.accepted_samples().front() == 100);
  CHECK(backend.accepted_samples().back() == 419);

  const auto stats = sink.stats();
  CHECK(stats.frames_consumed == 320);
  CHECK(stats.reference_blocks == 1);
  CHECK(collector.blocks.size() == 1);
  if (!collector.blocks.empty()) {
    const auto& block = collector.blocks.front();
    CHECK(block.kind == nexweave::backend::AlsaPlaybackReferenceKind::kAudio);
    CHECK(block.sequence == 0);
    CHECK(block.continuous);
    CHECK(block.write_begin_steady_ns > 0);
    CHECK(block.write_done_steady_ns >= block.write_begin_steady_ns);
    CHECK(block.interleaved_samples == backend.accepted_samples());
  }
  CHECK(sink.close().ok());
  CHECK(backend.stats().drain_calls == 1);
  CHECK(backend.stats().close_calls == 1);
}

void TestShortWriteAdvancesWithoutDuplication() {
  SinkFixture fixture;
  auto& sink = fixture.sink();
  auto& backend = fixture.backend();
  backend.push_write_result(PcmPlaybackWriteResult::Written(160, -1));
  backend.set_auto_accept(true);

  CHECK(sink.open().ok());
  CHECK(sink.write(MakeFrame(Ramp(320, 0))).ok());
  const auto accepted = backend.accepted_samples();
  CHECK(accepted.size() == 320);
  for (std::size_t index = 0; index < accepted.size(); ++index) {
    CHECK(accepted[index] == static_cast<std::int32_t>(index));
  }
  CHECK(sink.stats().short_writes == 1);
  CHECK(sink.close().ok());
}

void TestNativeFormatConversion() {
  // 48 kHz 立体声：一帧 16 kHz 单声道 320 样本应显式复制为左右声道并重采样到
  // 960 帧；每个 int32 样本都等于输入常数，证明不是只改元数据。
  SinkFixture stereo_fixture(MakeBackendConfig(48000, 2, PlaybackSampleFormat::kS16LE),
                             MakeSinkConfig(300, 8));
  auto& stereo_sink = stereo_fixture.sink();
  auto& stereo_backend = stereo_fixture.backend();
  CHECK(stereo_sink.open().ok());
  CHECK(stereo_sink.write(MakeFrame(Constant(320, 1500))).ok());
  const auto stereo_samples = stereo_backend.accepted_samples();
  CHECK(stereo_samples.size() == 960 * 2);
  for (std::int32_t sample : stereo_samples) {
    CHECK(sample == 1500);
  }
  CHECK(stereo_sink.stats().actual_sample_rate_hz == 48000);
  CHECK(stereo_sink.stats().actual_channels == 2);
  CHECK(stereo_sink.close().ok());

  // S24_LE 单声道：S16 输入应显式左移到 24 位有效范围。
  SinkFixture s24_fixture(MakeBackendConfig(16000, 1, PlaybackSampleFormat::kS24LE),
                          MakeSinkConfig(300, 8));
  auto& s24_sink = s24_fixture.sink();
  auto& s24_backend = s24_fixture.backend();
  CHECK(s24_sink.open().ok());
  CHECK(s24_sink.write(MakeFrame(Constant(320, -1234))).ok());
  const auto s24_samples = s24_backend.accepted_samples();
  CHECK(s24_samples.size() == 320);
  CHECK(s24_samples.front() == -1234 * 256);
  CHECK(s24_sink.stats().actual_format == PlaybackSampleFormat::kS24LE);
  CHECK(s24_sink.close().ok());
}

void TestUnderrunPublishesDiscontinuity() {
  SinkFixture fixture;
  auto& sink = fixture.sink();
  auto& backend = fixture.backend();
  ReferenceCollector collector;
  sink.set_playback_reference_callback(
      [&collector](const AlsaPlaybackReferenceBlock& block) { collector.Collect(block); });
  backend.push_status(PcmPlaybackWriteKind::kRecovered, ErrorCode::kNone, "fake underrun");

  CHECK(sink.open().ok());
  CHECK(sink.write(MakeFrame(Constant(320, 77))).ok());
  CHECK(backend.accepted_samples().size() == 320);
  CHECK(sink.stats().underruns == 1);
  CHECK(!collector.blocks.empty());
  bool saw_discontinuity = false;
  bool saw_audio_after_discontinuity = false;
  for (const auto& block : collector.blocks) {
    if (block.kind == nexweave::backend::AlsaPlaybackReferenceKind::kDiscontinuity) {
      saw_discontinuity = true;
    } else if (saw_discontinuity) {
      saw_audio_after_discontinuity = true;
      CHECK(!block.continuous);
    }
  }
  CHECK(saw_discontinuity);
  CHECK(saw_audio_after_discontinuity);
  CHECK(sink.close().ok());
}

void TestReconnectMarksReferenceDiscontinuous() {
  SinkFixture fixture;
  auto& sink = fixture.sink();
  auto& backend = fixture.backend();
  ReferenceCollector collector;
  sink.set_playback_reference_callback(
      [&collector](const AlsaPlaybackReferenceBlock& block) { collector.Collect(block); });
  backend.push_status(PcmPlaybackWriteKind::kDeviceFailure,
                      ErrorCode::kDeviceFailure, "fake 断开");

  CHECK(sink.open().ok());
  CHECK(sink.write(MakeFrame(Constant(320, 2000))).ok());
  CHECK(backend.accepted_samples().size() == 320);
  CHECK(sink.stats().reconnect_attempts == 1);
  CHECK(sink.stats().reconnect_successes == 1);
  bool saw_discontinuity = false;
  bool saw_audio = false;
  for (const auto& block : collector.blocks) {
    if (block.kind == nexweave::backend::AlsaPlaybackReferenceKind::kDiscontinuity) {
      saw_discontinuity = true;
    } else {
      saw_audio = true;
      if (saw_discontinuity) {
        CHECK(!block.continuous);
      }
    }
  }
  CHECK(saw_discontinuity);
  CHECK(saw_audio);
  CHECK(sink.close().ok());
}

void TestReconnectFailureStopsRound() {
  SinkFixture fixture;
  auto& sink = fixture.sink();
  auto& backend = fixture.backend();
  backend.set_recover_result(nexweave::domain::OperationResult::failure(
      ErrorCode::kDeviceFailure, "fake 恢复失败"));
  backend.push_status(PcmPlaybackWriteKind::kDeviceFailure,
                      ErrorCode::kDeviceFailure, "fake 断开");
  CHECK(sink.open().ok());
  const auto failed = sink.write(MakeFrame(Constant(320, 1)));
  CHECK(failed.error.code == ErrorCode::kDeviceFailure);
  // 失败轮次不能继续写；close/open 才能开始新轮。
  CHECK(sink.write(MakeFrame(Constant(320, 2))).error.code == ErrorCode::kDeviceFailure);
  CHECK(sink.close().ok());
}

void TestWriteTimeout() {
  SinkFixture fixture(MakeBackendConfig(), MakeSinkConfig(50, 8, 0, 0));
  auto& sink = fixture.sink();
  auto& backend = fixture.backend();
  backend.set_block_on_write(true);
  CHECK(sink.open().ok());
  const auto started = std::chrono::steady_clock::now();
  const auto result = sink.write(MakeFrame(Constant(320, 1)));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(result.error.code == ErrorCode::kTimeout);
  CHECK(elapsed < std::chrono::milliseconds(1500));
  CHECK(sink.close().ok());
}

void TestCancelWakesBlockingWriteAndReopens() {
  SinkFixture fixture(MakeBackendConfig(), MakeSinkConfig(5000, 8));
  auto& sink = fixture.sink();
  auto& backend = fixture.backend();
  backend.set_block_on_write(true);
  CHECK(sink.open().ok());
  nexweave::domain::OperationResult write_result;
  std::thread writer([&]() {
    write_result = sink.write(MakeFrame(Constant(320, 5)));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto cancel_started = std::chrono::steady_clock::now();
  CHECK(sink.cancel().ok());
  writer.join();
  const auto cancel_elapsed = std::chrono::steady_clock::now() - cancel_started;
  CHECK(write_result.error.code == ErrorCode::kCancelled);
  CHECK(cancel_elapsed < std::chrono::milliseconds(1500));
  CHECK(sink.write(MakeFrame(Constant(320, 6))).error.code == ErrorCode::kCancelled);
  CHECK(sink.open().error.code == ErrorCode::kAlreadyCompleted);
  CHECK(sink.close().ok());
  CHECK(backend.stats().close_calls == 1);

  backend.set_block_on_write(false);
  backend.set_auto_accept(true);
  CHECK(sink.open().ok());
  CHECK(sink.write(MakeFrame(Constant(320, 7))).ok());
  CHECK(sink.close().ok());
}

void TestCloseDrainsOrDrops() {
  // 正常 close 必须先排空设备；取消后的 close 不得重新排空或复活旧音频。
  SinkFixture normal(MakeBackendConfig(), MakeSinkConfig());
  auto& normal_sink = normal.sink();
  auto& normal_backend = normal.backend();
  CHECK(normal_sink.open().ok());
  CHECK(normal_sink.write(MakeFrame(Constant(320, 1))).ok());
  CHECK(normal_sink.close().ok());
  CHECK(normal_backend.stats().drain_calls == 1);
  CHECK(normal_backend.stats().close_calls == 1);
  CHECK(normal_sink.close().ok());

  SinkFixture cancelled(MakeBackendConfig(), MakeSinkConfig());
  auto& cancelled_sink = cancelled.sink();
  auto& cancelled_backend = cancelled.backend();
  CHECK(cancelled_sink.open().ok());
  CHECK(cancelled_sink.write(MakeFrame(Constant(320, 2))).ok());
  CHECK(cancelled_sink.cancel().ok());
  CHECK(cancelled_sink.close().ok());
  CHECK(cancelled_backend.stats().drain_calls == 0);
  CHECK(cancelled_backend.stats().close_calls == 1);
}

void TestDrainTimeoutIsReported() {
  SinkFixture fixture(MakeBackendConfig(), MakeSinkConfig(300, 8, 2, 4, 50));
  auto& sink = fixture.sink();
  auto& backend = fixture.backend();
  backend.set_drain_result(nexweave::domain::OperationResult::failure(
      ErrorCode::kTimeout, "fake 排空超时"));
  CHECK(sink.open().ok());
  CHECK(sink.write(MakeFrame(Constant(320, 1))).ok());
  const auto closed = sink.close();
  CHECK(closed.error.code == ErrorCode::kTimeout);
  CHECK(sink.close().ok());
}

void TestNoBusyLoopOnRepeatedRecovered() {
  SinkFixture fixture(MakeBackendConfig(), MakeSinkConfig(200, 3, 0, 1));
  auto& sink = fixture.sink();
  auto& backend = fixture.backend();
  backend.set_auto_accept(false);
  CHECK(sink.open().ok());
  const auto result = sink.write(MakeFrame(Constant(320, 1)));
  CHECK(!result.ok());
  CHECK(backend.stats().write_calls <= 2);
  CHECK(sink.close().ok());
}

}  // namespace

int main() {
  TestConfigValidation();
  TestOpenFailureAndDiagnostics();
  TestFixedFramePassThroughAndReference();
  TestShortWriteAdvancesWithoutDuplication();
  TestNativeFormatConversion();
  TestUnderrunPublishesDiscontinuity();
  TestReconnectMarksReferenceDiscontinuous();
  TestReconnectFailureStopsRound();
  TestWriteTimeout();
  TestCancelWakesBlockingWriteAndReopens();
  TestCloseDrainsOrDrops();
  TestDrainTimeoutIsReported();
  TestNoBusyLoopOnRepeatedRecovered();
  if (failures == 0) {
    std::printf("alsa_audio_sink_test OK\n");
    return 0;
  }
  std::printf("alsa_audio_sink_test failures=%d\n", failures);
  return 1;
}

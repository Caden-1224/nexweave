// ALSA 音频输入适配器行为测试。
//
// 测试只通过 capability::IAudioSource、受控 PCM 后端和适配器统计观察外部行为，
// 不访问真实声卡。保护的不变量：
//   - 设备缺失/打开失败有结构化诊断，失败后不会假装已经打开；
//   - 固定输出帧始终是 16 kHz/单声道/S16_LE/320 样本，短读不会变成短帧；
//   - 原生采样率、声道和编码由后端显式转换，跨块重采样状态正确；
//   - 取消是线性化点，能在有限等待内唤醒阻塞读，close/open 后才能开始下一轮；
//   - overrun/underrun 与设备断开不会空转，预算耗尽后明确失败；
//   - 重连成功会丢弃旧转换缓存，不会把断开前后的音频拼成连续帧；
//   - 重复 open/close、资源释放和长时间采集都不会泄漏旧轮次状态。
#include "alsa/alsa_audio_source.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../support/fake_pcm_capture_backend.hpp"

namespace {

using nexweave::backend::AlsaAudioSource;
using nexweave::backend::AlsaAudioSourceConfig;
using nexweave::backend::FakePcmCaptureBackend;
using nexweave::backend::PcmCaptureBackendConfig;
using nexweave::backend::PcmCaptureFormat;
using nexweave::backend::PcmCaptureReadKind;
using nexweave::backend::PcmSampleFormat;
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

PcmCaptureBackendConfig MakeBackendConfig(std::uint32_t rate = 16000,
                                          std::uint16_t channels = 1,
                                          PcmSampleFormat format = PcmSampleFormat::kS16LE) {
  PcmCaptureBackendConfig config;
  config.device = "fake:0";
  config.sample_rate_hz = rate;
  config.channels = channels;
  config.format = format;
  config.period_frames = 320;
  config.buffer_frames = 1280;
  config.read_poll_slice_ms = 10;
  return config;
}

AlsaAudioSourceConfig MakeSourceConfig(std::size_t timeout_ms = 300,
                                       std::size_t max_reads = 32,
                                       std::size_t reconnect_budget = 2,
                                       std::size_t max_recoveries = 4) {
  AlsaAudioSourceConfig config;
  config.read_timeout = std::chrono::milliseconds(timeout_ms);
  config.max_backend_reads_per_frame = max_reads;
  config.reconnect_budget = reconnect_budget;
  config.max_recoveries_per_frame = max_recoveries;
  return config;
}

PcmCaptureFormat MakeFormat(std::uint32_t rate, std::uint16_t channels,
                            PcmSampleFormat format) {
  PcmCaptureFormat output;
  output.sample_rate_hz = rate;
  output.channels = channels;
  output.format = format;
  return output;
}

std::vector<std::int32_t> MakeConstant(std::size_t frames, std::uint16_t channels,
                                       std::int32_t left, std::int32_t right) {
  std::vector<std::int32_t> samples;
  samples.reserve(frames * channels);
  for (std::size_t index = 0; index < frames; ++index) {
    samples.push_back(left);
    if (channels == 2) {
      samples.push_back(right);
    }
  }
  return samples;
}

bool ValidateOwnedFrame(const nexweave::domain::Result<AudioFrame>& result) {
  if (!result.ok()) {
    return false;
  }
  return nexweave::domain::validate_audio_frame(*result.value).ok();
}

class SourceFixture {
 public:
  SourceFixture(PcmCaptureBackendConfig backend_config, AlsaAudioSourceConfig source_config)
      : backend_config_(std::move(backend_config)),
        source_config_(std::move(source_config)) {
    auto backend = std::make_unique<FakePcmCaptureBackend>(backend_config_);
    backend_ = backend.get();
    source_ = std::make_unique<AlsaAudioSource>(source_config_, std::move(backend));
  }

  FakePcmCaptureBackend& backend() { return *backend_; }
  AlsaAudioSource& source() { return *source_; }

 private:
  PcmCaptureBackendConfig backend_config_;
  AlsaAudioSourceConfig source_config_;
  FakePcmCaptureBackend* backend_ = nullptr;
  std::unique_ptr<AlsaAudioSource> source_;
};

void TestConfigValidation() {
  // 边界：空设备、0 声道、0 period、buffer 小于 period、0 poll slice 都必须在
  // 打开前被拒绝；否则错误会以设备故障的形式晚到，难以区分配置错误和硬件故障。
  PcmCaptureBackendConfig backend = MakeBackendConfig();
  backend.device.clear();
  CHECK(!nexweave::backend::validate_pcm_capture_backend_config(backend).ok());

  backend = MakeBackendConfig();
  backend.channels = 0;
  CHECK(!nexweave::backend::validate_pcm_capture_backend_config(backend).ok());

  backend = MakeBackendConfig();
  backend.channels = 3;
  CHECK(!nexweave::backend::validate_pcm_capture_backend_config(backend).ok());

  backend = MakeBackendConfig();
  backend.period_frames = 0;
  CHECK(!nexweave::backend::validate_pcm_capture_backend_config(backend).ok());

  backend = MakeBackendConfig();
  backend.buffer_frames = backend.period_frames - 1;
  CHECK(!nexweave::backend::validate_pcm_capture_backend_config(backend).ok());

  backend = MakeBackendConfig();
  backend.read_poll_slice_ms = 0;
  CHECK(!nexweave::backend::validate_pcm_capture_backend_config(backend).ok());

  CHECK(nexweave::backend::validate_pcm_capture_backend_config(MakeBackendConfig()).ok());

  AlsaAudioSourceConfig source = MakeSourceConfig();
  source.read_timeout = std::chrono::milliseconds(0);
  CHECK(!nexweave::backend::validate_alsa_audio_source_config(source).ok());

  source = MakeSourceConfig();
  source.max_backend_reads_per_frame = 0;
  CHECK(!nexweave::backend::validate_alsa_audio_source_config(source).ok());

  // 0 重连/0 恢复是允许的显式禁用配置；它必须校验通过，再由 read 返回明确失败。
  source = MakeSourceConfig(300, 32, 0, 0);
  CHECK(nexweave::backend::validate_alsa_audio_source_config(source).ok());
}

void TestOpenFailureAndDiagnostics() {
  // 设备缺失：open 必须原样返回结构化设备错误，且未打开状态不能产生可读帧。
  auto backend = std::make_unique<FakePcmCaptureBackend>(MakeBackendConfig());
  backend->set_open_result(nexweave::domain::OperationResult::failure(
      ErrorCode::kDeviceFailure, "fake 设备缺失"));
  AlsaAudioSource source(MakeSourceConfig(), std::move(backend));
  const auto opened = source.open();
  CHECK(!opened.ok());
  CHECK(opened.error.code == ErrorCode::kDeviceFailure);
  CHECK(source.read().error.code == ErrorCode::kDeviceFailure);
  CHECK(source.close().ok());
  CHECK(source.close().ok());

  // 没有后端对象是配置错误，不能解引用空指针。
  AlsaAudioSource without_backend(MakeSourceConfig(), nullptr);
  CHECK(without_backend.open().error.code == ErrorCode::kInvalidInput);
}

void TestRepeatedOpenCloseAndCrossRoundState() {
  SourceFixture fixture(MakeBackendConfig(), MakeSourceConfig());
  auto& source = fixture.source();
  CHECK(source.open().ok());
  CHECK(source.open().error.code == ErrorCode::kAlreadyCompleted);
  CHECK(source.close().ok());
  CHECK(source.close().ok());
  CHECK(source.open().ok());

  fixture.backend().push_data(MakeFormat(16000, 1, PcmSampleFormat::kS16LE),
                              MakeConstant(320, 1, 1234, 0));
  const auto frame = source.read();
  CHECK(ValidateOwnedFrame(frame));
  CHECK(frame.value->samples.front() == 1234);
  CHECK(source.close().ok());
  CHECK(source.read().error.code == ErrorCode::kDeviceFailure);
}

void TestFixedFramePassThroughAndLongCapture() {
  // 成功路径：同一 16 kHz S16 块连续返回，每帧都必须通过统一音频契约。
  SourceFixture fixture(MakeBackendConfig(), MakeSourceConfig(500, 64));
  auto& source = fixture.source();
  auto& backend = fixture.backend();
  constexpr std::size_t kFrames = 200;
  for (std::size_t index = 0; index < kFrames; ++index) {
    std::vector<std::int32_t> samples(320);
    for (std::size_t offset = 0; offset < samples.size(); ++offset) {
      samples[offset] = static_cast<std::int32_t>((index * 320 + offset) % 10000) - 5000;
    }
    backend.push_data(MakeFormat(16000, 1, PcmSampleFormat::kS16LE),
                      std::move(samples));
  }
  CHECK(source.open().ok());
  for (std::size_t index = 0; index < kFrames; ++index) {
    const auto frame = source.read();
    CHECK(ValidateOwnedFrame(frame));
    if (frame.ok()) {
      CHECK(frame.value->samples.front() ==
            static_cast<std::int16_t>((index * 320) % 10000 - 5000));
      CHECK(frame.value->samples.back() ==
            static_cast<std::int16_t>((index * 320 + 319) % 10000 - 5000));
    }
  }
  CHECK(source.stats().frames_returned == kFrames);
  CHECK(source.close().ok());
  CHECK(backend.stats().close_calls == 1);
}

void TestShortReadsAccumulateIntoFullFrame() {
  // 短读：两次 160 样本的设备块必须拼成一个完整 320 样本帧，不能返回短帧。
  SourceFixture fixture(MakeBackendConfig(), MakeSourceConfig(500, 32));
  auto& source = fixture.source();
  auto& backend = fixture.backend();
  std::vector<std::int32_t> first(160);
  std::vector<std::int32_t> second(160);
  for (std::size_t index = 0; index < 160; ++index) {
    first[index] = static_cast<std::int32_t>(index);
    second[index] = static_cast<std::int32_t>(160 + index);
  }
  backend.push_data(MakeFormat(16000, 1, PcmSampleFormat::kS16LE), std::move(first));
  backend.push_data(MakeFormat(16000, 1, PcmSampleFormat::kS16LE), std::move(second));
  CHECK(source.open().ok());
  const auto frame = source.read();
  CHECK(ValidateOwnedFrame(frame));
  if (frame.ok()) {
    for (std::size_t index = 0; index < 320; ++index) {
      CHECK(frame.value->samples[index] == static_cast<std::int16_t>(index));
    }
  }
  CHECK(source.close().ok());
}

void TestRecoveryStatusContinuesOrFailsByBudget() {
  // overrun/underrun：后端报告 kRecovered 后适配器应重试；若恢复结果超过预算，
  // 必须有限失败，不能继续空转。
  SourceFixture fixture(MakeBackendConfig(), MakeSourceConfig(200, 8, 2, 1));
  auto& source = fixture.source();
  auto& backend = fixture.backend();
  CHECK(source.open().ok());
  backend.push_status(PcmCaptureReadKind::kRecovered, ErrorCode::kNone, "fake overrun");
  backend.push_data(MakeFormat(16000, 1, PcmSampleFormat::kS16LE),
                    MakeConstant(320, 1, 77, 0));
  const auto frame = source.read();
  CHECK(ValidateOwnedFrame(frame));
  if (frame.ok()) {
    CHECK(frame.value->samples.front() == 77);
  }

  backend.push_status(PcmCaptureReadKind::kRecovered, ErrorCode::kNone, "fake overrun");
  backend.push_status(PcmCaptureReadKind::kRecovered, ErrorCode::kNone, "fake overrun");
  const auto failed = source.read();
  CHECK(!failed.ok());
  CHECK(failed.error.code == ErrorCode::kDeviceFailure);
  CHECK(source.close().ok());
}

void TestS24ConversionAndStereoDownmix() {
  // 原生编码/声道转换：S24_LE 单声道按高 16 位转 S16_LE；立体声下混必须显式
  // 平均，不能只取左声道或改帧头。使用 16 kHz 原生率避免重采样干扰。
  SourceFixture s24_fixture(MakeBackendConfig(16000, 1, PcmSampleFormat::kS24LE),
                            MakeSourceConfig(300, 8));
  auto& s24_source = s24_fixture.source();
  auto& s24_backend = s24_fixture.backend();
  std::vector<std::int32_t> samples;
  samples.reserve(320);
  for (std::size_t index = 0; index < 320; ++index) {
    samples.push_back(index == 0 ? (8388607) : (index == 1 ? -8388608 : 0));
  }
  s24_backend.push_data(MakeFormat(16000, 1, PcmSampleFormat::kS24LE),
                        std::move(samples));
  CHECK(s24_source.open().ok());
  const auto s24_frame = s24_source.read();
  CHECK(ValidateOwnedFrame(s24_frame));
  if (s24_frame.ok()) {
    CHECK(s24_frame.value->samples[0] == 32767);
    CHECK(s24_frame.value->samples[1] == -32768);
    CHECK(s24_frame.value->samples[2] == 0);
  }
  CHECK(s24_source.close().ok());

  SourceFixture stereo_fixture(MakeBackendConfig(16000, 2, PcmSampleFormat::kS16LE),
                               MakeSourceConfig(300, 8));
  auto& stereo_source = stereo_fixture.source();
  auto& stereo_backend = stereo_fixture.backend();
  stereo_backend.push_data(MakeFormat(16000, 2, PcmSampleFormat::kS16LE),
                           MakeConstant(320, 2, 1000, 3000));
  CHECK(stereo_source.open().ok());
  const auto stereo_frame = stereo_source.read();
  CHECK(ValidateOwnedFrame(stereo_frame));
  if (stereo_frame.ok()) {
    CHECK(stereo_frame.value->samples.front() == 2000);
    CHECK(stereo_frame.value->samples.back() == 2000);
  }
  CHECK(stereo_source.close().ok());
}

void TestResamplerCrossBlockState() {
  // 重采样跨块状态：32 kHz 单声道转 16 kHz 时，第一块 320 帧只产生 160 个输出
  // 样本，第二块必须继续同一状态直到凑满 320；如果每块重置就会永远等不到完整帧。
  SourceFixture fixture(MakeBackendConfig(32000, 1, PcmSampleFormat::kS16LE),
                        MakeSourceConfig(500, 16));
  auto& source = fixture.source();
  auto& backend = fixture.backend();
  backend.push_data(MakeFormat(32000, 1, PcmSampleFormat::kS16LE),
                    MakeConstant(320, 1, 1500, 0));
  backend.push_data(MakeFormat(32000, 1, PcmSampleFormat::kS16LE),
                    MakeConstant(320, 1, 1500, 0));
  CHECK(source.open().ok());
  const auto frame = source.read();
  CHECK(ValidateOwnedFrame(frame));
  if (frame.ok()) {
    for (std::int16_t sample : frame.value->samples) {
      CHECK(sample == 1500);
    }
  }
  CHECK(source.stats().frames_returned == 1);
  CHECK(source.close().ok());
}

void TestReconnectDiscardsOldConversionCache() {
  // 断开恢复：旧格式的 160 个中间输出样本在 recover 成功后必须丢弃，不能和
  // 新格式的音频拼成一帧。这里让 recover 成功并返回新的 16 kHz 数据。
  SourceFixture fixture(MakeBackendConfig(32000, 1, PcmSampleFormat::kS16LE),
                        MakeSourceConfig(500, 16, 1, 2));
  auto& source = fixture.source();
  auto& backend = fixture.backend();
  backend.push_data(MakeFormat(32000, 1, PcmSampleFormat::kS16LE),
                    MakeConstant(320, 1, 1000, 0));
  backend.push_status(PcmCaptureReadKind::kDeviceFailure,
                      ErrorCode::kDeviceFailure, "fake 断开");
  backend.push_data(MakeFormat(16000, 1, PcmSampleFormat::kS16LE),
                    MakeConstant(320, 1, 2000, 0));
  CHECK(source.open().ok());
  const auto frame = source.read();
  CHECK(ValidateOwnedFrame(frame));
  if (frame.ok()) {
    for (std::int16_t sample : frame.value->samples) {
      CHECK(sample == 2000);
    }
  }
  const auto stats = source.stats();
  CHECK(stats.reconnect_attempts == 1);
  CHECK(stats.reconnect_successes == 1);
  CHECK(stats.samples_discarded_on_recovery >= 160);
  CHECK(source.close().ok());
}

void TestReconnectFailureAndDisabledBudget() {
  // 恢复失败/禁用预算：必须返回设备失败，不能继续读旧后端结果或无限重连。
  SourceFixture fixture(MakeBackendConfig(), MakeSourceConfig(200, 8, 1, 2));
  auto& source = fixture.source();
  auto& backend = fixture.backend();
  backend.set_recover_result(nexweave::domain::OperationResult::failure(
      ErrorCode::kDeviceFailure, "fake 恢复失败"));
  backend.push_status(PcmCaptureReadKind::kDeviceFailure,
                      ErrorCode::kDeviceFailure, "fake 断开");
  CHECK(source.open().ok());
  const auto failed = source.read();
  CHECK(failed.error.code == ErrorCode::kDeviceFailure);
  CHECK(source.close().ok());

  SourceFixture no_reconnect(MakeBackendConfig(), MakeSourceConfig(200, 8, 0, 2));
  auto& no_reconnect_source = no_reconnect.source();
  auto& no_reconnect_backend = no_reconnect.backend();
  no_reconnect_backend.push_status(PcmCaptureReadKind::kDeviceFailure,
                                   ErrorCode::kDeviceFailure, "fake 断开");
  CHECK(no_reconnect_source.open().ok());
  const auto no_reconnect_failed = no_reconnect_source.read();
  CHECK(no_reconnect_failed.error.code == ErrorCode::kDeviceFailure);
  CHECK(no_reconnect_source.close().ok());
}

void TestReadTimeoutAndCancelWakeBlockingRead() {
  // 超时：阻塞后端在 50 ms 内没有数据时应返回 kTimeout，不能永久挂起。
  SourceFixture timeout_fixture(MakeBackendConfig(), MakeSourceConfig(50, 8));
  auto& timeout_source = timeout_fixture.source();
  timeout_fixture.backend().set_block_on_read(true);
  CHECK(timeout_source.open().ok());
  const auto started = std::chrono::steady_clock::now();
  const auto timed_out = timeout_source.read();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(timed_out.error.code == ErrorCode::kTimeout);
  CHECK(elapsed < std::chrono::milliseconds(1500));
  CHECK(timeout_source.close().ok());

  // 取消唤醒：read 在另一个线程阻塞，cancel 必须在有限等待内让它返回 kCancelled。
  SourceFixture cancel_fixture(MakeBackendConfig(), MakeSourceConfig(5000, 8));
  auto& cancel_source = cancel_fixture.source();
  cancel_fixture.backend().set_block_on_read(true);
  CHECK(cancel_source.open().ok());
  nexweave::domain::Result<nexweave::domain::AudioFrame> read_result;
  std::thread reader([&]() { read_result = cancel_source.read(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto cancel_started = std::chrono::steady_clock::now();
  CHECK(cancel_source.cancel().ok());
  reader.join();
  const auto cancel_elapsed = std::chrono::steady_clock::now() - cancel_started;
  CHECK(read_result.error.code == ErrorCode::kCancelled);
  CHECK(cancel_elapsed < std::chrono::milliseconds(1500));
  CHECK(cancel_source.read().error.code == ErrorCode::kCancelled);
  // 尚未 close 时重复 open 不得清除取消状态；按既有 IAudioSource 轮次语义返回
  // kAlreadyCompleted，真正的下一轮必须 close 后 open。
  CHECK(cancel_source.open().error.code == ErrorCode::kAlreadyCompleted);
  CHECK(cancel_source.close().ok());
  CHECK(cancel_source.open().ok());
  cancel_fixture.backend().push_data(MakeFormat(16000, 1, PcmSampleFormat::kS16LE),
                                     MakeConstant(320, 1, 4321, 0));
  const auto next_round = cancel_source.read();
  CHECK(ValidateOwnedFrame(next_round));
  CHECK(cancel_source.close().ok());
}

void TestCloseWakesBlockingReadAndReleasesBackend() {
  // close 释放：close 必须唤醒阻塞 read 并调用后端 close；读线程退出后对象可安全
  // 进入下一轮。这里不要求 read 返回成功，只要求有限退出且错误可诊断。
  SourceFixture fixture(MakeBackendConfig(), MakeSourceConfig(5000, 8));
  auto& source = fixture.source();
  auto& backend = fixture.backend();
  backend.set_block_on_read(true);
  CHECK(source.open().ok());
  nexweave::domain::Result<nexweave::domain::AudioFrame> read_result;
  std::thread reader([&]() { read_result = source.read(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto close_started = std::chrono::steady_clock::now();
  CHECK(source.close().ok());
  reader.join();
  const auto close_elapsed = std::chrono::steady_clock::now() - close_started;
  CHECK(read_result.error.code == ErrorCode::kDeviceFailure);
  CHECK(close_elapsed < std::chrono::milliseconds(1500));
  CHECK(backend.stats().close_calls == 1);
  CHECK(source.close().ok());
  CHECK(source.open().ok());
  CHECK(source.close().ok());
}

void TestNoBusyLoopWhenBackendKeepsReturningTimeouts() {
  // 空转保护：后端连续返回 kTimeout 时，read 调用的最大后端次数必须受预算限制。
  SourceFixture fixture(MakeBackendConfig(), MakeSourceConfig(200, 3));
  auto& source = fixture.source();
  auto& backend = fixture.backend();
  CHECK(source.open().ok());
  for (int index = 0; index < 10; ++index) {
    backend.push_status(PcmCaptureReadKind::kTimeout, ErrorCode::kTimeout, "fake timeout");
  }
  const auto result = source.read();
  CHECK(result.error.code == ErrorCode::kTimeout);
  CHECK(backend.stats().read_calls <= 4);
  CHECK(source.close().ok());
}

}  // namespace

int main() {
  TestConfigValidation();
  TestOpenFailureAndDiagnostics();
  TestRepeatedOpenCloseAndCrossRoundState();
  TestFixedFramePassThroughAndLongCapture();
  TestShortReadsAccumulateIntoFullFrame();
  TestRecoveryStatusContinuesOrFailsByBudget();
  TestS24ConversionAndStereoDownmix();
  TestResamplerCrossBlockState();
  TestReconnectDiscardsOldConversionCache();
  TestReconnectFailureAndDisabledBudget();
  TestReadTimeoutAndCancelWakeBlockingRead();
  TestCloseWakesBlockingReadAndReleasesBackend();
  TestNoBusyLoopWhenBackendKeepsReturningTimeouts();
  if (failures == 0) {
    std::printf("alsa_audio_source_test OK\n");
    return 0;
  }
  std::printf("alsa_audio_source_test failures=%d\n", failures);
  return 1;
}

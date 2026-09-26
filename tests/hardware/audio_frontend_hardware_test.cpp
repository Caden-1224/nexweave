// RK3576 全双工音频前端硬件入口。
//
// 本入口只使用显式命令行参数打开真实 ALSA 设备，并通过 DuplexAudioFrontend
// 观察处理后音频、AEC 状态与资源账目。它不把“设备能同时打开”报告成 AEC 已收敛，
// 也不做人耳质量或回声残留自动判定：声学指标一律输出 not_measured，由任务 49
// 在固定设备、音量、距离和录音条件下另做对比。
#include "audio_frontend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "alsa/alsa_pcm_capture_backend.hpp"
#include "alsa/alsa_pcm_playback_backend.hpp"
#include "webrtc/webrtc_audio_processor.hpp"

namespace {

using nexweave::backend::AlsaPcmCaptureBackend;
using nexweave::backend::AlsaPcmPlaybackBackend;
using nexweave::backend::PcmCaptureBackendConfig;
using nexweave::backend::PcmPlaybackBackendConfig;
using nexweave::backend::PcmSampleFormat;
using nexweave::backend::PlaybackSampleFormat;
using nexweave::backend::WebrtcAudioProcessor;
using nexweave::domain::AudioFrame;
using nexweave::domain::ErrorCode;
using nexweave::runtime::AudioFrontendConfig;
using nexweave::runtime::AudioPipelineState;
using nexweave::runtime::AudioProcessorConfig;
using nexweave::runtime::DuplexAudioFrontend;

struct Options {
  std::string mode = "near";
  int repeat = 1;
  std::string capture_device = "hw:0,0";
  std::string playback_device = "hw:0,0";
  std::uint32_t capture_rate = 16000;
  std::uint16_t capture_channels = 1;
  std::uint32_t playback_rate = 44100;
  std::uint16_t playback_channels = 2;
};

AudioFrame MakeToneFrame(std::size_t frame_index, double amplitude) {
  constexpr double kPi = 3.14159265358979323846;
  std::vector<std::int16_t> samples(320);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double phase = 2.0 * kPi * 440.0 *
                         (static_cast<double>(frame_index * samples.size() + i) /
                          16000.0);
    samples[i] = static_cast<std::int16_t>(
        std::lround(std::sin(phase) * amplitude));
  }
  const auto made = AudioFrame::from_samples(samples);
  return made.value;
}

PcmCaptureBackendConfig MakeCaptureConfig(const Options& options) {
  PcmCaptureBackendConfig config;
  config.device = options.capture_device;
  config.sample_rate_hz = options.capture_rate;
  config.channels = options.capture_channels;
  config.format = PcmSampleFormat::kS16LE;
  config.period_frames = 320;
  config.buffer_frames = 1280;
  config.read_poll_slice_ms = 20;
  return config;
}

PcmPlaybackBackendConfig MakePlaybackConfig(const Options& options) {
  PcmPlaybackBackendConfig config;
  config.device = options.playback_device;
  config.sample_rate_hz = options.playback_rate;
  config.channels = options.playback_channels;
  config.format = PlaybackSampleFormat::kS16LE;
  config.period_frames = std::max<std::size_t>(1, options.playback_rate / 100);
  config.buffer_frames = config.period_frames * 4;
  config.write_poll_slice_ms = 20;
  config.drain_poll_slice_ms = 10;
  return config;
}

AudioFrontendConfig MakeFrontendConfig() {
  AudioFrontendConfig config;
  config.source.read_timeout = std::chrono::milliseconds(1000);
  config.source.max_backend_reads_per_frame = 128;
  config.source.reconnect_budget = 1;
  config.source.max_recoveries_per_frame = 16;
  config.sink.write_timeout = std::chrono::milliseconds(1000);
  config.sink.max_backend_writes_per_frame = 256;
  config.sink.reconnect_budget = 1;
  config.sink.max_recoveries_per_frame = 16;
  config.sink.drain_timeout = std::chrono::milliseconds(5000);
  config.processed_frame_queue_capacity = 128;
  config.render_reference_sample_capacity = 96000;
  config.reference_schedule_tolerance = std::chrono::milliseconds(5);
  config.max_reference_lateness = std::chrono::milliseconds(100);
  config.reconnect_retry_interval = std::chrono::milliseconds(200);
  config.reconnect_total_budget = std::chrono::milliseconds(3000);
  return config;
}

AudioProcessorConfig MakeProcessorConfig() {
  AudioProcessorConfig config;
  config.stream_delay_ms = 40;
  config.noise_suppression_level = 2;
  config.min_render_frames_for_convergence = 500;
  config.min_capture_frames_for_convergence = 500;
  config.min_delay_estimates_for_convergence = 1;
  config.max_poor_delay_fraction = 0.25F;
  config.require_erle_for_convergence = false;
  return config;
}

std::unique_ptr<DuplexAudioFrontend> MakeFrontend(const Options& options) {
  return std::make_unique<DuplexAudioFrontend>(
      MakeFrontendConfig(),
      std::make_unique<AlsaPcmCaptureBackend>(MakeCaptureConfig(options)),
      std::make_unique<AlsaPcmPlaybackBackend>(MakePlaybackConfig(options)),
      std::make_unique<WebrtcAudioProcessor>(MakeProcessorConfig()));
}

void PrintStats(const char* prefix, const DuplexAudioFrontend& frontend) {
  const auto stats = frontend.stats();
  std::printf("%s_capture_state=%d\n", prefix,
              static_cast<int>(stats.capture_state));
  std::printf("%s_playback_state=%d\n", prefix,
              static_cast<int>(stats.playback_state));
  std::printf("%s_aec_state=%d\n", prefix,
              static_cast<int>(stats.processor.state));
  std::printf("%s_capture_frames=%llu\n", prefix,
              static_cast<unsigned long long>(stats.capture_frames));
  std::printf("%s_playback_frames=%llu\n", prefix,
              static_cast<unsigned long long>(stats.playback_frames));
  std::printf("%s_reference_blocks=%llu\n", prefix,
              static_cast<unsigned long long>(stats.render_reference_blocks));
  std::printf("%s_reference_discontinuities=%llu\n", prefix,
              static_cast<unsigned long long>(
                  stats.render_reference_discontinuities));
  std::printf("%s_processor_resets=%llu\n", prefix,
              static_cast<unsigned long long>(stats.processor_resets));
  std::printf("%s_processor_failures=%llu\n", prefix,
              static_cast<unsigned long long>(stats.processor_failures));
  std::printf("%s_render_frames=%llu\n", prefix,
              static_cast<unsigned long long>(stats.processor.render_frames));
  std::printf("%s_capture_processor_frames=%llu\n", prefix,
              static_cast<unsigned long long>(stats.processor.capture_frames));
  std::printf("%s_delay_median_ms=%d\n", prefix,
              static_cast<int>(stats.processor.delay_median_ms));
  std::printf("%s_fraction_poor_delays=%.6f\n", prefix,
              static_cast<double>(stats.processor.fraction_poor_delays));
  std::printf("%s_erle_average_db=%d\n", prefix,
              static_cast<int>(stats.processor.erle_average_db));
  std::printf("%s_late_reference_samples=%llu\n", prefix,
              static_cast<unsigned long long>(
                  stats.render_reference_late_samples));
  std::printf("%s_reference_overflows=%llu\n", prefix,
              static_cast<unsigned long long>(stats.render_reference_overflows));
  std::printf("%s_processor_last_reset=%s\n", prefix,
              stats.processor.last_reset_reason.c_str());
  std::printf("%s_acoustic_metrics=not_measured\n", prefix);
}

int RunNear(const Options& options) {
  auto frontend = MakeFrontend(options);
  const auto opened = frontend->source().open();
  if (!opened.ok()) {
    std::printf("open_error=%s\n", opened.error.message.c_str());
    return 1;
  }
  std::size_t frames = 0;
  std::size_t failures = 0;
  for (int i = 0; i < options.repeat; ++i) {
    const auto read = frontend->source().read();
    if (!read.ok()) {
      ++failures;
      std::printf("read_error_code=%d message=%s\n",
                  static_cast<int>(read.error.code), read.error.message.c_str());
      break;
    }
    ++frames;
  }
  PrintStats("near", *frontend);
  (void)frontend->source().close();
  std::printf("near_frames=%zu failures=%zu\n", frames, failures);
  return failures == 0 ? 0 : 1;
}

int RunPlayOnly(const Options& options) {
  auto frontend = MakeFrontend(options);
  const auto opened = frontend->sink().open();
  if (!opened.ok()) {
    std::printf("open_error=%s\n", opened.error.message.c_str());
    return 1;
  }
  std::size_t failures = 0;
  for (int i = 0; i < options.repeat; ++i) {
    const auto written = frontend->sink().write(
        MakeToneFrame(static_cast<std::size_t>(i), 8000.0));
    if (!written.ok()) {
      ++failures;
      std::printf("write_error_code=%d message=%s\n",
                  static_cast<int>(written.error.code),
                  written.error.message.c_str());
      break;
    }
  }
  PrintStats("play", *frontend);
  (void)frontend->sink().close();
  std::printf("play_frames=%d failures=%zu\n", options.repeat, failures);
  return failures == 0 ? 0 : 1;
}

int RunDoubleTalk(const Options& options) {
  auto frontend = MakeFrontend(options);
  const auto source_opened = frontend->source().open();
  if (!source_opened.ok()) {
    std::printf("capture_open_error=%s\n", source_opened.error.message.c_str());
    return 1;
  }
  const auto sink_opened = frontend->sink().open();
  if (!sink_opened.ok()) {
    std::printf("playback_open_error=%s\n", sink_opened.error.message.c_str());
    (void)frontend->source().close();
    return 1;
  }
  std::size_t play_failures = 0;
  std::size_t read_failures = 0;
  constexpr int kPlaybackFramesPerRead = 5;
  std::size_t playback_index = 0;
  for (int i = 0; i < options.repeat; ++i) {
    // 先填满约 100 ms 播放缓冲，再读一个 20 ms 近端帧，模拟真实双讲时
    // 播放流持续供给、采集流同时推进。若每写一帧就读一次，20 ms 写间隔会
    // 让测试夹具自己制造 underrun，把设备连续性误算到 AEC 状态上。
    for (int write_index = 0; write_index < kPlaybackFramesPerRead;
         ++write_index) {
      const auto written = frontend->sink().write(
          MakeToneFrame(playback_index++, 4000.0));
      if (!written.ok()) {
        ++play_failures;
        std::printf("write_error_code=%d message=%s\n",
                    static_cast<int>(written.error.code),
                    written.error.message.c_str());
        break;
      }
    }
    if (play_failures != 0) {
      break;
    }
    const auto read = frontend->source().read();
    if (!read.ok() && read.error.code != ErrorCode::kTimeout) {
      ++read_failures;
      std::printf("read_error_code=%d message=%s\n",
                  static_cast<int>(read.error.code), read.error.message.c_str());
      break;
    }
  }
  PrintStats("double", *frontend);
  (void)frontend->sink().close();
  (void)frontend->source().close();
  std::printf("double_play_failures=%zu double_read_failures=%zu\n",
              play_failures, read_failures);
  return play_failures == 0 && read_failures == 0 ? 0 : 1;
}

int RunCancel(const Options& options) {
  std::size_t failures = 0;
  for (int round = 0; round < options.repeat; ++round) {
    auto frontend = MakeFrontend(options);
    const auto opened = frontend->source().open();
    if (!opened.ok()) {
      ++failures;
      continue;
    }
    // 真实设备可能已经缓存可读音频，因此这里验收“取消状态建立后 read 必须
    // 返回 kCancelled”这一线性化事实；阻塞读唤醒由受控 Fake 用例覆盖，不能
    // 依赖真实麦克风在任意机器上一定刚好阻塞。
    (void)frontend->source().cancel();
    const auto read_result = frontend->source().read();
    if (read_result.ok() || read_result.error.code != ErrorCode::kCancelled) {
      ++failures;
    }
    (void)frontend->source().close();
  }
  std::printf("cancel_rounds=%d failures=%zu timeout_ms=1000\n", options.repeat,
              failures);
  return failures == 0 ? 0 : 1;
}

int RunLifecycle(const Options& options) {
  std::size_t failures = 0;
  for (int round = 0; round < options.repeat; ++round) {
    auto frontend = MakeFrontend(options);
    const auto source_opened = frontend->source().open();
    const auto sink_opened = frontend->sink().open();
    if (!source_opened.ok() || !sink_opened.ok()) {
      ++failures;
      (void)frontend->source().close();
      (void)frontend->sink().close();
      continue;
    }
    const auto written =
        frontend->sink().write(MakeToneFrame(static_cast<std::size_t>(round), 2000.0));
    if (!written.ok()) {
      ++failures;
    }
    (void)frontend->sink().close();
    (void)frontend->source().close();
  }
  std::printf("lifecycle_rounds=%d failures=%zu timeout_ms=1000\n",
              options.repeat, failures);
  return failures == 0 ? 0 : 1;
}

bool ParseUnsigned(const char* text, std::uint32_t* value) {
  if (text == nullptr || value == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text, &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *value = static_cast<std::uint32_t>(parsed);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (argc >= 2) {
    options.mode = argv[1];
  }
  if (argc >= 3) {
    options.repeat = std::max(1, std::atoi(argv[2]));
  }
  if (argc >= 4) {
    options.capture_device = argv[3];
  }
  if (argc >= 5) {
    options.playback_device = argv[4];
  }
  std::uint32_t value = 0;
  if (argc >= 6 && ParseUnsigned(argv[5], &value)) {
    options.capture_rate = value;
  }
  if (argc >= 7 && ParseUnsigned(argv[6], &value)) {
    options.capture_channels = static_cast<std::uint16_t>(value);
  }
  if (argc >= 8 && ParseUnsigned(argv[7], &value)) {
    options.playback_rate = value;
  }
  if (argc >= 9 && ParseUnsigned(argv[8], &value)) {
    options.playback_channels = static_cast<std::uint16_t>(value);
  }

  if (options.mode == "near") {
    return RunNear(options);
  }
  if (options.mode == "play") {
    return RunPlayOnly(options);
  }
  if (options.mode == "double") {
    return RunDoubleTalk(options);
  }
  if (options.mode == "cancel") {
    return RunCancel(options);
  }
  if (options.mode == "lifecycle") {
    return RunLifecycle(options);
  }
  std::printf("unsupported_mode=%s\n", options.mode.c_str());
  return 2;
}

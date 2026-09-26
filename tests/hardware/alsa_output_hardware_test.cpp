// RK3576 板端真实 ALSA 音频输出适配器验收夹具。
//
// 夹具只通过 capability::IAudioSink 驱动 AlsaAudioSink，并通过
// AlsaPcmPlaybackBackend 打开真实声卡。设备名、原生采样率、声道、格式、
// period/buffer、写超时、重连预算和排空预算全部由命令行显式注入。
//
// 支持模式：
//   probe    打开/查询实际格式/关闭；
//   tone     生成 440 Hz 16 kHz 单声道测试音，写入若干秒并在 close 时排空；
//   cancel   持续写入期间取消，验证有限唤醒、丢弃和 close 不复活旧音频；
//   invalid  用错误设备名验证打开失败诊断与 close 幂等。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "alsa/alsa_audio_sink.hpp"
#include "alsa/alsa_pcm_playback_backend.hpp"

using namespace nexweave;
using nexweave::backend::AlsaAudioSink;
using nexweave::backend::AlsaAudioSinkConfig;
using nexweave::backend::AlsaPcmPlaybackBackend;
using nexweave::backend::AlsaPlaybackReferenceBlock;
using nexweave::backend::AlsaPlaybackReferenceKind;
using nexweave::backend::PcmPlaybackBackendConfig;
using nexweave::backend::PlaybackSampleFormat;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      ++g_failures;                                                             \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #condition \
                << std::endl;                                                   \
    }                                                                           \
  } while (false)

struct Options {
  std::string device;
  std::uint32_t rate = 16000;
  std::uint16_t channels = 1;
  PlaybackSampleFormat format = PlaybackSampleFormat::kS16LE;
  std::size_t period_frames = 320;
  std::size_t buffer_frames = 1280;
  std::size_t write_poll_slice_ms = 20;
  std::size_t drain_poll_slice_ms = 10;
  std::size_t write_timeout_ms = 1000;
  std::size_t max_writes = 64;
  std::size_t reconnect_budget = 2;
  std::size_t max_recoveries = 8;
  std::size_t drain_timeout_ms = 5000;
  std::string mode = "tone";
  std::size_t seconds = 1;
  std::size_t repeat = 1;
  std::string wav;
  std::string invalid_device = "hw:99,0";
};

bool ParseSize(const char* text, std::size_t* value) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text, &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *value = static_cast<std::size_t>(parsed);
  return true;
}

bool ParseFormat(const std::string& text, PlaybackSampleFormat* format) {
  if (text == "s16" || text == "S16_LE") {
    *format = PlaybackSampleFormat::kS16LE;
    return true;
  }
  if (text == "s24" || text == "S24_LE") {
    *format = PlaybackSampleFormat::kS24LE;
    return true;
  }
  return false;
}

bool ParseOptions(int argc, char** argv, Options* options) {
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    auto next = [&](const char** value) -> bool {
      if (index + 1 >= argc) {
        std::cerr << "参数 " << flag << " 缺少取值" << std::endl;
        return false;
      }
      *value = argv[++index];
      return true;
    };
    const char* value = nullptr;
    if (flag == "--device") {
      if (!next(&value)) return false;
      options->device = value;
    } else if (flag == "--rate") {
      if (!next(&value)) return false;
      options->rate = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (flag == "--channels") {
      if (!next(&value)) return false;
      options->channels = static_cast<std::uint16_t>(std::strtoul(value, nullptr, 10));
    } else if (flag == "--format") {
      if (!next(&value)) return false;
      if (!ParseFormat(value, &options->format)) {
        std::cerr << "不支持的 --format: " << value << std::endl;
        return false;
      }
    } else if (flag == "--period") {
      if (!next(&value) || !ParseSize(value, &options->period_frames)) return false;
    } else if (flag == "--buffer") {
      if (!next(&value) || !ParseSize(value, &options->buffer_frames)) return false;
    } else if (flag == "--write-poll-slice-ms") {
      if (!next(&value) || !ParseSize(value, &options->write_poll_slice_ms)) return false;
    } else if (flag == "--drain-poll-slice-ms") {
      if (!next(&value) || !ParseSize(value, &options->drain_poll_slice_ms)) return false;
    } else if (flag == "--write-timeout-ms") {
      if (!next(&value) || !ParseSize(value, &options->write_timeout_ms)) return false;
    } else if (flag == "--max-writes") {
      if (!next(&value) || !ParseSize(value, &options->max_writes)) return false;
    } else if (flag == "--reconnect-budget") {
      if (!next(&value) || !ParseSize(value, &options->reconnect_budget)) return false;
    } else if (flag == "--max-recoveries") {
      if (!next(&value) || !ParseSize(value, &options->max_recoveries)) return false;
    } else if (flag == "--drain-timeout-ms") {
      if (!next(&value) || !ParseSize(value, &options->drain_timeout_ms)) return false;
    } else if (flag == "--mode") {
      if (!next(&value)) return false;
      options->mode = value;
    } else if (flag == "--seconds") {
      if (!next(&value) || !ParseSize(value, &options->seconds)) return false;
    } else if (flag == "--repeat") {
      if (!next(&value) || !ParseSize(value, &options->repeat)) return false;
    } else if (flag == "--wav") {
      if (!next(&value)) return false;
      options->wav = value;
    } else if (flag == "--invalid-device") {
      if (!next(&value)) return false;
      options->invalid_device = value;
    } else if (flag == "--help") {
      return false;
    } else {
      std::cerr << "未知参数: " << flag << std::endl;
      return false;
    }
  }
  return true;
}

PcmPlaybackBackendConfig MakeBackendConfig(const Options& options) {
  PcmPlaybackBackendConfig config;
  config.device = options.device;
  config.sample_rate_hz = options.rate;
  config.channels = options.channels;
  config.format = options.format;
  config.period_frames = options.period_frames;
  config.buffer_frames = options.buffer_frames;
  config.write_poll_slice_ms = options.write_poll_slice_ms;
  config.drain_poll_slice_ms = options.drain_poll_slice_ms;
  return config;
}

AlsaAudioSinkConfig MakeSinkConfig(const Options& options) {
  AlsaAudioSinkConfig config;
  config.write_timeout = std::chrono::milliseconds(options.write_timeout_ms);
  config.max_backend_writes_per_frame = options.max_writes;
  config.reconnect_budget = options.reconnect_budget;
  config.max_recoveries_per_frame = options.max_recoveries;
  config.drain_timeout = std::chrono::milliseconds(options.drain_timeout_ms);
  return config;
}

std::unique_ptr<AlsaAudioSink> CreateSink(const Options& options) {
  auto backend = std::make_unique<AlsaPcmPlaybackBackend>(MakeBackendConfig(options));
  return std::make_unique<AlsaAudioSink>(MakeSinkConfig(options), std::move(backend));
}

domain::AudioFrame MakeToneFrame(std::size_t frame_index, int tone_hz) {
  std::vector<std::int16_t> samples(domain::kAudioFrameSamples);
  const double base_phase = 2.0 * 3.14159265358979323846 * tone_hz /
                            static_cast<double>(domain::kAudioSampleRateHz);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const double phase =
        base_phase * static_cast<double>(frame_index * domain::kAudioFrameSamples + index);
    samples[index] = static_cast<std::int16_t>(std::sin(phase) * 12000.0);
  }
  const auto frame = domain::AudioFrame::from_samples(samples);
  if (!frame.ok()) {
    ++g_failures;
    return domain::AudioFrame{};
  }
  return frame.value;
}

bool WritePcmWav(const std::string& path,
                 const std::vector<std::int16_t>& samples) {
  if (path.empty()) {
    return true;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    std::cerr << "无法写入 WAV: " << path << std::endl;
    return false;
  }
  const std::uint32_t data_bytes =
      static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
  const std::uint32_t riff_size = 36U + data_bytes;
  const std::uint16_t channels = 1U;
  const std::uint32_t sample_rate = domain::kAudioSampleRateHz;
  const std::uint16_t bits_per_sample = 16U;
  const std::uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8U;
  const std::uint16_t block_align =
      static_cast<std::uint16_t>(channels * bits_per_sample / 8U);
  output.write("RIFF", 4);
  output.write(reinterpret_cast<const char*>(&riff_size), sizeof(riff_size));
  output.write("WAVEfmt ", 8);
  const std::uint32_t fmt_size = 16U;
  const std::uint16_t audio_format = 1U;
  output.write(reinterpret_cast<const char*>(&fmt_size), sizeof(fmt_size));
  output.write(reinterpret_cast<const char*>(&audio_format), sizeof(audio_format));
  output.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
  output.write(reinterpret_cast<const char*>(&sample_rate), sizeof(sample_rate));
  output.write(reinterpret_cast<const char*>(&byte_rate), sizeof(byte_rate));
  output.write(reinterpret_cast<const char*>(&block_align), sizeof(block_align));
  output.write(reinterpret_cast<const char*>(&bits_per_sample), sizeof(bits_per_sample));
  output.write("data", 4);
  output.write(reinterpret_cast<const char*>(&data_bytes), sizeof(data_bytes));
  output.write(reinterpret_cast<const char*>(samples.data()),
               static_cast<std::streamsize>(data_bytes));
  return output.good();
}

void PrintFormat(const char* label, const backend::PcmPlaybackFormat& format) {
  const char* name =
      format.format == PlaybackSampleFormat::kS16LE ? "S16_LE" : "S24_LE";
  std::cout << label << " rate=" << format.sample_rate_hz
            << " channels=" << format.channels << " format=" << name << std::endl;
}

bool RunProbe(const Options& options) {
  auto sink = CreateSink(options);
  const auto opened = sink->open();
  if (!opened.ok()) {
    std::cerr << "probe open 失败 code=" << static_cast<int>(opened.error.code)
              << " message=" << opened.error.message << std::endl;
    return false;
  }
  PrintFormat("actual", sink->actual_playback_format());
  const auto stats = sink->stats();
  std::cout << "probe open_success=" << stats.open_successes
            << " actual_period_frames=" << stats.actual_period_frames
            << " actual_buffer_frames=" << stats.actual_buffer_frames << std::endl;
  CHECK(sink->close().ok());
  return true;
}

bool RunTone(const Options& options) {
  const std::size_t frames =
      std::max<std::size_t>(1, options.seconds) * domain::kAudioSampleRateHz /
      domain::kAudioFrameSamples;
  int successful_runs = 0;
  std::vector<std::int16_t> sent_samples;
  for (std::size_t run = 0; run < std::max<std::size_t>(1, options.repeat); ++run) {
    auto sink = CreateSink(options);
    std::size_t reference_blocks = 0;
    std::size_t reference_samples = 0;
    std::size_t discontinuities = 0;
    sink->set_playback_reference_callback(
        [&](const AlsaPlaybackReferenceBlock& block) {
          if (block.kind == AlsaPlaybackReferenceKind::kAudio) {
            ++reference_blocks;
            reference_samples += block.interleaved_samples.size();
          } else {
            ++discontinuities;
          }
        });

    const auto opened = sink->open();
    if (!opened.ok()) {
      std::cerr << "tone open 失败 code=" << static_cast<int>(opened.error.code)
                << " message=" << opened.error.message << std::endl;
      return false;
    }
    bool failed = false;
    std::vector<std::int16_t> samples;
    samples.reserve(frames * domain::kAudioFrameSamples);
    const auto write_started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < frames; ++index) {
      const auto frame = MakeToneFrame(index, 440);
      const auto written = sink->write(frame);
      if (!written.ok()) {
        std::cerr << "tone write 失败 code=" << static_cast<int>(written.error.code)
                  << " message=" << written.error.message << std::endl;
        failed = true;
        break;
      }
      samples.insert(samples.end(), frame.samples.begin(), frame.samples.end());
    }
    const auto write_elapsed = std::chrono::steady_clock::now() - write_started;
    if (!failed) {
      ++successful_runs;
    }
    if (run == 0) {
      sent_samples = samples;
      std::cout << "tone_write_elapsed_ms="
                << std::chrono::duration_cast<std::chrono::milliseconds>(write_elapsed)
                       .count()
                << std::endl;
      const auto stats = sink->stats();
      std::cout << "tone frames_written=" << frames
                << " reference_blocks=" << reference_blocks
                << " reference_samples=" << reference_samples
                << " discontinuities=" << discontinuities
                << " frames_consumed=" << stats.frames_consumed
                << " actual_period_frames=" << stats.actual_period_frames
                << " actual_buffer_frames=" << stats.actual_buffer_frames
                << std::endl;
      PrintFormat("actual", sink->actual_playback_format());
    }
    const auto close_started = std::chrono::steady_clock::now();
    const auto closed = sink->close();
    const auto drain_elapsed = std::chrono::steady_clock::now() - close_started;
    std::cout << "close elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(drain_elapsed)
                     .count()
              << " code=" << static_cast<int>(closed.error.code)
              << " message=" << closed.error.message << std::endl;
    CHECK(closed.ok());
  }
  if (!options.wav.empty() && !WritePcmWav(options.wav, sent_samples)) {
    return false;
  }
  std::cout << "tone_repeat=" << successful_runs << "/"
            << std::max<std::size_t>(1, options.repeat) << std::endl;
  CHECK(successful_runs == static_cast<int>(std::max<std::size_t>(1, options.repeat)));
  return successful_runs > 0;
}

bool RunCancel(const Options& options) {
  auto sink = CreateSink(options);
  CHECK(sink->open().ok());
  std::atomic<bool> stop{false};
  domain::OperationResult write_result;
  std::thread writer([&]() {
    std::size_t index = 0;
    while (!stop.load()) {
      const auto written = sink->write(MakeToneFrame(index, 440));
      if (!written.ok()) {
        write_result = written;
        break;
      }
      ++index;
      if (index > 100000) {
        break;
      }
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto cancel_started = std::chrono::steady_clock::now();
  CHECK(sink->cancel().ok());
  stop.store(true);
  writer.join();
  const auto cancel_elapsed = std::chrono::steady_clock::now() - cancel_started;
  std::cout << "cancel write_code=" << static_cast<int>(write_result.error.code)
            << " elapsed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(cancel_elapsed)
                   .count()
            << std::endl;
  CHECK(write_result.error.code == domain::ErrorCode::kCancelled);
  CHECK(cancel_elapsed < std::chrono::milliseconds(options.write_timeout_ms + 500));
  const auto close_started = std::chrono::steady_clock::now();
  CHECK(sink->close().ok());
  const auto close_elapsed = std::chrono::steady_clock::now() - close_started;
  std::cout << "cancel close_elapsed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(close_elapsed)
                   .count()
            << std::endl;
  CHECK(close_elapsed < std::chrono::milliseconds(options.drain_timeout_ms + 500));

  // 关闭后可以重新打开并写入一帧，证明取消不会永久锁死设备。
  CHECK(sink->open().ok());
  CHECK(sink->write(MakeToneFrame(0, 440)).ok());
  CHECK(sink->close().ok());
  return true;
}

bool RunInvalid(const Options& options) {
  Options invalid = options;
  invalid.device = options.invalid_device;
  auto sink = CreateSink(invalid);
  const auto opened = sink->open();
  std::cout << "invalid open_code=" << static_cast<int>(opened.error.code)
            << " message=" << opened.error.message << std::endl;
  CHECK(!opened.ok());
  CHECK(!opened.error.message.empty());
  CHECK(sink->close().ok());
  CHECK(sink->close().ok());
  return true;
}

void PrintUsage(const char* program) {
  std::cerr << "用法: " << program
            << " --device <alsa> [--rate 16000] [--channels 1] [--format s16|s24]"
               " [--period 320] [--buffer 1280] [--write-poll-slice-ms 20]"
               " [--drain-poll-slice-ms 10] [--write-timeout-ms 1000]"
               " [--max-writes 64] [--reconnect-budget 2] [--max-recoveries 8]"
               " [--drain-timeout-ms 5000] [--mode probe|tone|cancel|invalid]"
               " [--seconds 1] [--repeat 1] [--wav path]"
               " [--invalid-device hw:99,0]"
            << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    PrintUsage(argv[0]);
    return 2;
  }
  if (options.device.empty()) {
    std::cerr << "必须显式提供 --device" << std::endl;
    PrintUsage(argv[0]);
    return 2;
  }

  bool mode_ok = false;
  if (options.mode == "probe") {
    mode_ok = RunProbe(options);
  } else if (options.mode == "tone") {
    mode_ok = RunTone(options);
  } else if (options.mode == "cancel") {
    mode_ok = RunCancel(options);
  } else if (options.mode == "invalid") {
    mode_ok = RunInvalid(options);
  } else {
    std::cerr << "未知 mode: " << options.mode << std::endl;
    PrintUsage(argv[0]);
    return 2;
  }

  if (!mode_ok || g_failures != 0) {
    std::cerr << "alsa_output_hardware_test failures=" << g_failures << std::endl;
    return 1;
  }
  std::cout << "alsa_output_hardware_test OK" << std::endl;
  return 0;
}

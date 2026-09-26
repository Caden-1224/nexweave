// RK3576 板端真实 ALSA 音频输入适配器验收夹具。
//
// 夹具只通过 capability::IAudioSource 驱动 AlsaAudioSource，并通过
// AlsaPcmCaptureBackend 打开真实声卡。设备名、原生采样率、声道、格式、
// period/buffer、读超时和重连预算全部由命令行显式注入，源码不写死板端设备名。
//
// 支持模式：
//   probe    打开/查询实际格式/关闭，验证设备探测与打开；
//   capture  连续采集若干秒，校验每帧 16 kHz/单声道/S16_LE/320 样本并保存 WAV；
//   cancel   在阻塞 read 期间取消，验证有限唤醒；close/open 后继续采集；
//   invalid  用错误设备名验证打开失败诊断与 close 幂等。
#include <algorithm>
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

#include "alsa/alsa_audio_source.hpp"
#include "alsa/alsa_pcm_capture_backend.hpp"

using namespace nexweave;
using nexweave::backend::AlsaAudioSource;
using nexweave::backend::AlsaAudioSourceConfig;
using nexweave::backend::AlsaPcmCaptureBackend;
using nexweave::backend::PcmCaptureBackendConfig;
using nexweave::backend::PcmSampleFormat;

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
  PcmSampleFormat format = PcmSampleFormat::kS16LE;
  std::size_t period_frames = 320;
  std::size_t buffer_frames = 1280;
  std::size_t poll_slice_ms = 20;
  std::size_t read_timeout_ms = 1000;
  std::size_t max_reads = 64;
  std::size_t reconnect_budget = 2;
  std::size_t max_recoveries = 8;
  std::string mode = "capture";
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

bool ParseFormat(const std::string& text, PcmSampleFormat* format) {
  if (text == "s16" || text == "S16_LE") {
    *format = PcmSampleFormat::kS16LE;
    return true;
  }
  if (text == "s24" || text == "S24_LE") {
    *format = PcmSampleFormat::kS24LE;
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
    } else if (flag == "--poll-slice-ms") {
      if (!next(&value) || !ParseSize(value, &options->poll_slice_ms)) return false;
    } else if (flag == "--read-timeout-ms") {
      if (!next(&value) || !ParseSize(value, &options->read_timeout_ms)) return false;
    } else if (flag == "--max-reads") {
      if (!next(&value) || !ParseSize(value, &options->max_reads)) return false;
    } else if (flag == "--reconnect-budget") {
      if (!next(&value) || !ParseSize(value, &options->reconnect_budget)) return false;
    } else if (flag == "--max-recoveries") {
      if (!next(&value) || !ParseSize(value, &options->max_recoveries)) return false;
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

PcmCaptureBackendConfig MakeBackendConfig(const Options& options) {
  PcmCaptureBackendConfig config;
  config.device = options.device;
  config.sample_rate_hz = options.rate;
  config.channels = options.channels;
  config.format = options.format;
  config.period_frames = options.period_frames;
  config.buffer_frames = options.buffer_frames;
  config.read_poll_slice_ms = options.poll_slice_ms;
  return config;
}

AlsaAudioSourceConfig MakeSourceConfig(const Options& options) {
  AlsaAudioSourceConfig config;
  config.read_timeout = std::chrono::milliseconds(options.read_timeout_ms);
  config.max_backend_reads_per_frame = options.max_reads;
  config.reconnect_budget = options.reconnect_budget;
  config.max_recoveries_per_frame = options.max_recoveries;
  return config;
}

std::unique_ptr<AlsaAudioSource> CreateSource(const Options& options) {
  auto backend = std::make_unique<AlsaPcmCaptureBackend>(MakeBackendConfig(options));
  return std::make_unique<AlsaAudioSource>(MakeSourceConfig(options), std::move(backend));
}

bool WriteWav16(const std::string& path, const std::vector<std::int16_t>& samples) {
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

void PrintFormat(const char* label, const backend::PcmCaptureFormat& format) {
  const char* format_name = format.format == PcmSampleFormat::kS16LE ? "S16_LE" : "S24_LE";
  std::cout << label << " rate=" << format.sample_rate_hz
            << " channels=" << format.channels
            << " format=" << format_name << std::endl;
}

bool RunProbe(const Options& options) {
  auto source = CreateSource(options);
  const auto opened = source->open();
  if (!opened.ok()) {
    std::cerr << "probe open 失败 code=" << static_cast<int>(opened.error.code)
              << " message=" << opened.error.message << std::endl;
    return false;
  }
  PrintFormat("actual", source->actual_capture_format());
  const auto stats = source->stats();
  std::cout << "probe open_success=" << stats.open_successes
            << " source_reads=" << stats.read_calls << std::endl;
  CHECK(source->close().ok());
  return true;
}

bool RunCapture(const Options& options, const std::string& wav) {
  const std::size_t frames_to_read =
      std::max<std::size_t>(1, options.seconds) * domain::kAudioSampleRateHz /
      domain::kAudioFrameSamples;
  int successful_runs = 0;
  std::vector<std::int16_t> all_samples;
  for (std::size_t run = 0; run < std::max<std::size_t>(1, options.repeat); ++run) {
    auto source = CreateSource(options);
    const auto opened = source->open();
    if (!opened.ok()) {
      std::cerr << "capture open 失败 code=" << static_cast<int>(opened.error.code)
                << " message=" << opened.error.message << std::endl;
      return false;
    }
    std::size_t read_frames = 0;
    bool failed = false;
    std::vector<std::int16_t> samples;
    samples.reserve(frames_to_read * domain::kAudioFrameSamples);
    for (std::size_t index = 0; index < frames_to_read; ++index) {
      const auto frame = source->read();
      if (!frame.ok()) {
        std::cerr << "capture read 失败 code="
                  << static_cast<int>(frame.error.code)
                  << " message=" << frame.error.message << std::endl;
        failed = true;
        break;
      }
      if (!domain::validate_audio_frame(*frame.value).ok()) {
        std::cerr << "捕获帧不符合 16 kHz/单声道/S16_LE/320" << std::endl;
        failed = true;
        break;
      }
      samples.insert(samples.end(), frame.value->samples.begin(),
                     frame.value->samples.end());
      ++read_frames;
    }
    if (!failed) {
      ++successful_runs;
    }
    if (run == 0) {
      all_samples = samples;
      const auto stats = source->stats();
      std::cout << "capture frames=" << read_frames
                << " samples=" << samples.size()
                << " source_frames_returned=" << stats.frames_returned
                << " backend_reads=" << stats.backend_reads
                << " reconnect_successes=" << stats.reconnect_successes
                << std::endl;
      PrintFormat("actual", source->actual_capture_format());
    }
    CHECK(source->close().ok());
  }
  if (!wav.empty() && !WriteWav16(wav, all_samples)) {
    return false;
  }
  std::cout << "capture_repeat=" << successful_runs << "/"
            << std::max<std::size_t>(1, options.repeat) << std::endl;
  CHECK(successful_runs == static_cast<int>(std::max<std::size_t>(1, options.repeat)));
  return successful_runs > 0;
}

bool RunCancel(const Options& options) {
  auto source = CreateSource(options);
  const auto opened = source->open();
  if (!opened.ok()) {
    std::cerr << "cancel open 失败 code=" << static_cast<int>(opened.error.code)
              << " message=" << opened.error.message << std::endl;
    return false;
  }
  domain::Result<domain::AudioFrame> read_result;
  std::thread reader([&]() { read_result = source->read(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto cancel_started = std::chrono::steady_clock::now();
  CHECK(source->cancel().ok());
  reader.join();
  const auto cancel_elapsed = std::chrono::steady_clock::now() - cancel_started;
  std::cout << "cancel read_code=" << static_cast<int>(read_result.error.code)
            << " elapsed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(cancel_elapsed).count()
            << std::endl;
  CHECK(read_result.error.code == domain::ErrorCode::kCancelled);
  CHECK(cancel_elapsed < std::chrono::milliseconds(options.read_timeout_ms + 500));
  CHECK(source->close().ok());

  // 取消后必须 close/open 才能开始下一轮。取消测试使用大 period 确保读真的阻塞；
  // 恢复验证改回单个 20 ms 帧的 period，才能在同一声卡上快速采到一帧并证明收敛。
  Options recovery_options = options;
  recovery_options.period_frames = domain::kAudioFrameSamples;
  recovery_options.buffer_frames = domain::kAudioFrameSamples * 4;
  auto recovery_source = CreateSource(recovery_options);
  const auto reopened = recovery_source->open();
  CHECK(reopened.ok());
  const auto frame = recovery_source->read();
  CHECK(frame.ok());
  if (frame.ok()) {
    CHECK(domain::validate_audio_frame(*frame.value).ok());
  }
  CHECK(recovery_source->close().ok());
  return true;
}

bool RunInvalid(const Options& options) {
  Options invalid = options;
  invalid.device = options.invalid_device;
  auto source = CreateSource(invalid);
  const auto opened = source->open();
  std::cout << "invalid open_code=" << static_cast<int>(opened.error.code)
            << " message=" << opened.error.message << std::endl;
  CHECK(!opened.ok());
  CHECK(!opened.error.message.empty());
  CHECK(source->close().ok());
  CHECK(source->close().ok());
  return true;
}

void PrintUsage(const char* program) {
  std::cerr << "用法: " << program
            << " --device <alsa> [--rate 16000] [--channels 1] [--format s16|s24]"
               " [--period 320] [--buffer 1280] [--poll-slice-ms 20]"
               " [--read-timeout-ms 1000] [--max-reads 64]"
               " [--reconnect-budget 2] [--max-recoveries 8]"
               " [--mode probe|capture|cancel|invalid] [--seconds 1] [--repeat 1]"
               " [--wav path] [--invalid-device hw:99,0]"
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
  } else if (options.mode == "capture") {
    mode_ok = RunCapture(options, options.wav);
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
    std::cerr << "alsa_input_hardware_test failures=" << g_failures << std::endl;
    return 1;
  }
  std::cout << "alsa_input_hardware_test OK" << std::endl;
  return 0;
}

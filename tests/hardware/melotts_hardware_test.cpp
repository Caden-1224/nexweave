// RK3576 板端真实 MeloTTS 离线适配器验收夹具。
//
// 只通过 capability::ITts 或 MeloTtsTts 统计快照驱动真实模型：ONNX Runtime CPU
// 编码器 + RKNN NPU 解码器，文本前端从命令行注入。覆盖正常中英混合、长文本、
// 取消恢复、回调异常、无效模型路径与可选 WAV 输出；不把板端路径写死在仓库。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "melotts_adapter_factory.hpp"

using namespace nexweave;
using namespace nexweave::backend;

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

constexpr char kShortText[] = "帮我把 Gateway 的日志导出一下。";

std::string LongText() {
  std::string text;
  for (int index = 0; index < 8; ++index) {
    text += "帮我把 Gateway 的日志导出一下。";
  }
  return text;
}

std::int64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
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
  const std::uint16_t block_align = static_cast<std::uint16_t>(channels * bits_per_sample / 8U);
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

bool ValidateFrame(const domain::AudioFrame& frame) {
  return frame.sample_rate_hz == domain::kAudioSampleRateHz &&
         frame.channels == domain::kAudioChannels &&
         frame.format == domain::AudioSampleFormat::kS16LE &&
         frame.samples.size() == domain::kAudioFrameSamples;
}

struct RunResult {
  bool success = false;
  bool first_callback_before_return = false;
  std::vector<domain::AudioFrame> frames;
  std::vector<std::int16_t> samples;
  MeloTtsStatistics stats;
  std::int64_t first_callback_us = -1;
  std::int64_t total_us = -1;
};

RunResult RunSynthesize(MeloTtsTts& tts, const std::string& text) {
  RunResult result;
  bool returned = false;
  const std::int64_t started = now_us();
  const auto registered = tts.set_callback([&](const domain::AudioFrame& frame) {
    if (!returned && result.first_callback_us < 0) {
      result.first_callback_before_return = true;
      result.first_callback_us = now_us() - started;
    }
    if (!ValidateFrame(frame)) {
      ++g_failures;
      std::cerr << "FAIL 帧格式不符合 16 kHz/单声道/S16_LE/320" << std::endl;
      return;
    }
    result.frames.push_back(frame);
    result.samples.insert(result.samples.end(), frame.samples.begin(), frame.samples.end());
  });
  if (!registered.ok()) {
    std::cerr << "set_callback 失败 code=" << static_cast<int>(registered.error.code)
              << std::endl;
    return result;
  }
  const auto synthesized = tts.synthesize(text);
  returned = true;
  result.total_us = now_us() - started;
  result.success = synthesized.ok();
  result.stats = tts.last_statistics();
  if (!synthesized.ok()) {
    std::cerr << "synthesize 失败 code=" << static_cast<int>(synthesized.error.code)
              << " message=" << synthesized.error.message << std::endl;
  }
  return result;
}

std::unique_ptr<MeloTtsTts> CreateAdapter(const backend::MeloTtsConfig& config) {
  auto created = backend::create_melotts_tts_adapter(config);
  if (!created.ok()) {
    std::cerr << "创建 MeloTTS 适配器失败 code="
              << static_cast<int>(created.error.code)
              << " message=" << created.error.message << std::endl;
    return nullptr;
  }
  return std::move(*created.value);
}

backend::MeloTtsConfig MakeConfig(int argc, char** argv) {
  backend::MeloTtsConfig config;
  if (argc >= 6) {
    config.encoder_model_path = argv[1];
    config.decoder_model_path = argv[2];
    config.lexicon_path = argv[3];
    config.tokens_path = argv[4];
    config.g_path = argv[5];
  }
  return config;
}

bool PrintAndCheck(RunResult& result, const char* label, bool require_output) {
  if (!result.success) {
    ++g_failures;
    return false;
  }
  CHECK(result.first_callback_before_return);
  CHECK(!result.frames.empty());
  if (require_output) {
    CHECK(result.frames.size() > 1U);
  }
  CHECK(result.stats.encoder_runs >= 1U);
  CHECK(result.stats.decoder_runs >= 1U);
  CHECK(result.stats.resampled_samples > 0U);
  CHECK(result.stats.delivered_frames == result.frames.size());
  CHECK(result.stats.tail_kind != MeloTtsStatistics::TailKind::kNoOutput);
  std::cout << "  " << label << " frames=" << result.frames.size()
            << " resampled_samples=" << result.stats.resampled_samples
            << " native_samples=" << result.stats.native_samples
            << " chunks=" << result.stats.text_chunks
            << " first_callback_ms=" << result.first_callback_us / 1000.0
            << " total_ms=" << result.total_us / 1000.0
            << " tail_valid=" << result.stats.last_frame_valid_samples
            << std::endl;
  return true;
}

void RunSuccess(const backend::MeloTtsConfig& config, int repeat, const std::string& wav) {
  auto tts = CreateAdapter(config);
  if (!tts) {
    ++g_failures;
    return;
  }
  int success = 0;
  for (int index = 0; index < repeat; ++index) {
    const int failures_before = g_failures;
    RunResult result = RunSynthesize(*tts, kShortText);
    if (PrintAndCheck(result, "success", true) && g_failures == failures_before) {
      ++success;
    }
    if (index == 0 && !wav.empty() && !WriteWav16(wav, result.samples)) {
      ++g_failures;
    }
  }
  std::cout << "success_repeat=" << success << "/" << repeat << std::endl;
  CHECK(success == repeat);
}

void RunLong(const backend::MeloTtsConfig& config, const std::string& wav) {
  auto tts = CreateAdapter(config);
  if (!tts) {
    ++g_failures;
    return;
  }
  RunResult result = RunSynthesize(*tts, LongText());
  if (PrintAndCheck(result, "long", true)) {
    CHECK(result.stats.text_chunks >= 1U);
    if (!wav.empty() && !WriteWav16(wav, result.samples)) {
      ++g_failures;
    }
  }
}

void RunCancel(const backend::MeloTtsConfig& config, int repeat) {
  auto tts = CreateAdapter(config);
  if (!tts) {
    ++g_failures;
    return;
  }
  int success = 0;
  for (int index = 0; index < repeat; ++index) {
    const int failures_before = g_failures;
    int delivered = 0;
    CHECK(tts->set_callback([&](const domain::AudioFrame& frame) {
             (void)frame;
             ++delivered;
             if (delivered == 5) {
               (void)tts->cancel();
             }
           }).ok());
    const auto canceled = tts->synthesize(kShortText);
    CHECK(canceled.error.code == domain::ErrorCode::kCancelled);
    CHECK(delivered == 5);

    // 取消后重新注册是唯一恢复入口；新轮次必须从空状态完整成功。
    RunResult recovered = RunSynthesize(*tts, kShortText);
    if (PrintAndCheck(recovered, "cancel-recovery", true) &&
        g_failures == failures_before) {
      ++success;
    }
  }
  std::cout << "cancel_repeat=" << success << "/" << repeat << std::endl;
  CHECK(success == repeat);
}

void RunCallbackFailure(const backend::MeloTtsConfig& config) {
  auto tts = CreateAdapter(config);
  if (!tts) {
    ++g_failures;
    return;
  }
  CHECK(tts->set_callback([](const domain::AudioFrame&) {
          throw std::runtime_error("硬件测试注入回调异常");
        }).ok());
  bool threw = false;
  try {
    (void)tts->synthesize(kShortText);
  } catch (const std::runtime_error& error) {
    threw = std::string(error.what()) == "硬件测试注入回调异常";
  }
  CHECK(threw);
  CHECK(tts->synthesize(kShortText).error.code == domain::ErrorCode::kCancelled);
  RunResult recovered = RunSynthesize(*tts, kShortText);
  PrintAndCheck(recovered, "callback-failure-recovery", true);
}

void RunInvalid(const backend::MeloTtsConfig& config) {
  backend::MeloTtsConfig invalid = config;
  invalid.encoder_model_path = "/tmp/does-not-exist-melotts-encoder.onnx";
  auto tts = CreateAdapter(invalid);
  CHECK(!tts);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) {
    std::cerr << "用法: " << argv[0]
              << " <encoder.onnx> <decoder.rknn> <lexicon.txt> <tokens.txt> <g.bin>"
                 " [success|long|cancel|callback-failure|invalid] [repeat] [wav]"
              << std::endl;
    return 2;
  }
  const std::string mode = argc >= 7 ? argv[6] : "success";
  const int repeat = argc >= 8 ? std::max(1, std::atoi(argv[7])) : 1;
  const std::string wav = argc >= 9 ? argv[8] : "";
  const backend::MeloTtsConfig config = MakeConfig(argc, argv);

  if (mode == "success") {
    RunSuccess(config, repeat, wav);
  } else if (mode == "long") {
    RunLong(config, wav);
  } else if (mode == "cancel") {
    RunCancel(config, repeat);
  } else if (mode == "callback-failure") {
    RunCallbackFailure(config);
  } else if (mode == "invalid") {
    RunInvalid(config);
  } else {
    std::cerr << "未知 mode: " << mode << std::endl;
    return 2;
  }

  if (g_failures != 0) {
    std::cerr << "melotts_hardware_test failures=" << g_failures << std::endl;
    return 1;
  }
  std::cout << "melotts_hardware_test OK" << std::endl;
  return 0;
}

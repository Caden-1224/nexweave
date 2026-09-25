// RK3576 板端真实 RKNN Zipformer 验收夹具。
//
// 它只通过公共 IAsr 接口驱动适配器：读取 16 kHz/单声道/S16_LE WAV，按 320 样本
// 帧喂入，收集 partial/final/done 与单调时间。模型、词表和 WAV 都由命令行注入，
// 不依赖当前工作目录，也不把板端路径写入仓库。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rknn_zipformer_asr.hpp"

using namespace nexweave;
using backend::RknnZipformerAsr;

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

struct WavData {
  std::vector<std::int16_t> samples;
  std::uint32_t sample_rate = 0;
  std::uint16_t channels = 0;
  std::uint16_t bits_per_sample = 0;
};

std::uint32_t read_u32(std::istream& input) {
  std::uint32_t value = 0;
  input.read(reinterpret_cast<char*>(&value), sizeof(value));
  return value;
}

std::uint16_t read_u16(std::istream& input) {
  std::uint16_t value = 0;
  input.read(reinterpret_cast<char*>(&value), sizeof(value));
  return value;
}

bool ReadWav(const std::string& path, WavData& output) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return false;
  }
  char riff[4] = {};
  input.read(riff, sizeof(riff));
  const std::uint32_t riff_size = read_u32(input);
  char wave[4] = {};
  input.read(wave, sizeof(wave));
  if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
    return false;
  }
  (void)riff_size;
  bool format_ok = false;
  const std::uint32_t kMaxDataBytes = 512U * 1024U * 1024U;
  while (input.good()) {
    char chunk_id[4] = {};
    input.read(chunk_id, sizeof(chunk_id));
    if (!input.good()) {
      break;
    }
    const std::uint32_t chunk_size = read_u32(input);
    if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
      if (chunk_size < 16U) {
        return false;
      }
      const std::uint16_t audio_format = read_u16(input);
      output.channels = read_u16(input);
      output.sample_rate = read_u32(input);
      (void)read_u32(input);
      (void)read_u16(input);
      output.bits_per_sample = read_u16(input);
      if (chunk_size > 16U) {
        input.seekg(static_cast<std::streamoff>(chunk_size - 16U), std::ios::cur);
      }
      format_ok = audio_format == 1U && output.channels == 1U &&
                  output.sample_rate == 16000U &&
                  output.bits_per_sample == 16U;
      if (!format_ok) {
        return false;
      }
    } else if (std::memcmp(chunk_id, "data", 4) == 0) {
      if (!format_ok || chunk_size == 0U || chunk_size > kMaxDataBytes) {
        return false;
      }
      output.samples.resize(chunk_size / 2U);
      input.read(reinterpret_cast<char*>(output.samples.data()),
                 static_cast<std::streamsize>(chunk_size));
      return input.good();
    } else {
      const std::uint32_t padded = chunk_size + (chunk_size % 2U);
      input.seekg(static_cast<std::streamoff>(padded), std::ios::cur);
    }
  }
  return false;
}

std::int64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

domain::AudioFrame MakeFrame(const std::vector<std::int16_t>& samples,
                             std::size_t offset,
                             std::size_t* consumed) {
  std::vector<std::int16_t> chunk(domain::kAudioFrameSamples, 0);
  const std::size_t count =
      std::min(domain::kAudioFrameSamples, samples.size() - offset);
  std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(offset), count,
              chunk.begin());
  *consumed = count;
  const auto frame = domain::AudioFrame::from_samples(chunk);
  return frame.value;
}

struct EventLog {
  std::vector<capability::TextEvent> events;
  std::int64_t first_partial_us = -1;
  std::int64_t input_endpoint_us = -1;
  std::int64_t final_us = -1;
};

bool FeedAll(capability::IAsr& asr,
             const std::vector<std::int16_t>& samples,
             EventLog* log,
             bool finalize = true) {
  std::size_t offset = 0;
  while (offset < samples.size()) {
    std::size_t consumed = 0;
    const auto frame = MakeFrame(samples, offset, &consumed);
    offset += consumed;
    const bool is_last = finalize && offset >= samples.size();
    if (is_last && log != nullptr && log->input_endpoint_us < 0) {
      log->input_endpoint_us = now_us();
    }
    const auto fed = asr.feed(frame, is_last);
    if (!fed.ok()) {
      std::cerr << "feed 失败: code=" << static_cast<int>(fed.error.code)
                << " message=" << fed.error.message << std::endl;
      return false;
    }
    if (log != nullptr && log->final_us >= 0 && !is_last) {
      std::cerr << "final 后仍收到未结束帧" << std::endl;
      return false;
    }
  }
  return true;
}

std::unique_ptr<RknnZipformerAsr> CreateAsr(const backend::RknnZipformerConfig& config) {
  auto created = backend::RknnZipformerAsr::create(config);
  if (!created.ok()) {
    std::cerr << "创建 RKNN ASR 失败: code="
              << static_cast<int>(created.error.code)
              << " message=" << created.error.message << std::endl;
    return nullptr;
  }
  return std::move(*created.value);
}

backend::RknnZipformerConfig MakeConfig(int argc, char** argv) {
  backend::RknnZipformerConfig config;
  if (argc >= 6) {
    config.encoder_model_path = argv[1];
    config.decoder_model_path = argv[2];
    config.joiner_model_path = argv[3];
    config.vocab_path = argv[4];
  }
  return config;
}

bool RunSuccessCase(const backend::RknnZipformerConfig& config,
                    const std::vector<std::int16_t>& samples,
                    const std::string& expected_text,
                    bool enforce_expected) {
  auto asr = CreateAsr(config);
  if (!asr) {
    ++g_failures;
    return false;
  }
  std::cout << "  sdk_api_version=" << asr->sdk_api_version()
            << " driver_version=" << asr->sdk_driver_version() << std::endl;
  EventLog log;
  const auto started = now_us();
  const auto registered = asr->set_callback([&](const capability::TextEvent& event) {
    log.events.push_back(event);
    if (event.kind == capability::TextEventKind::kPartial &&
        log.first_partial_us < 0) {
      log.first_partial_us = now_us();
    }
    if (event.kind == capability::TextEventKind::kFinal) {
      log.final_us = now_us();
    }
  });
  if (!registered.ok()) {
    std::cerr << "set_callback 失败" << std::endl;
    ++g_failures;
    return false;
  }
  if (!FeedAll(*asr, samples, &log)) {
    ++g_failures;
    return false;
  }

  std::size_t partials = 0;
  std::size_t finals = 0;
  std::size_t dones = 0;
  std::string final_text;
  for (const auto& event : log.events) {
    if (event.kind == capability::TextEventKind::kPartial) {
      ++partials;
    } else if (event.kind == capability::TextEventKind::kFinal) {
      ++finals;
      final_text = event.text;
    } else if (event.kind == capability::TextEventKind::kDone) {
      ++dones;
    }
  }
  CHECK(partials >= 1);
  CHECK(finals == 1);
  CHECK(dones == 1);
  CHECK(log.first_partial_us >= started);
  CHECK(log.input_endpoint_us >= started);
  CHECK(log.final_us >= log.input_endpoint_us);
  if (enforce_expected) {
    CHECK(final_text == expected_text);
  }
  if (!final_text.empty()) {
    std::cout << "  final_text=" << final_text << std::endl;
  }
  std::cout << "  first_partial_ms="
            << (log.first_partial_us - started) / 1000.0
            << " input_endpoint_to_final_ms="
            << (log.final_us - log.input_endpoint_us) / 1000.0
            << " total_to_final_ms=" << (log.final_us - started) / 1000.0
            << " audio_ms=" << samples.size() * 1000U / 16000U << std::endl;
  return g_failures == 0;
}

void RunSuccessRepeat(const backend::RknnZipformerConfig& config,
                      const std::vector<std::int16_t>& samples,
                      int repeat) {
  const std::string expected = "对我做了介绍那么我想说的是大家如果对我的研究感兴趣呢";
  int success = 0;
  for (int index = 0; index < repeat; ++index) {
    const int before = g_failures;
    if (RunSuccessCase(config, samples, expected, true) && g_failures == before) {
      ++success;
    }
  }
  std::cout << "success_repeat=" << success << "/" << repeat << std::endl;
  CHECK(success == repeat);
}

void RunCancelCase(const backend::RknnZipformerConfig& config,
                   const std::vector<std::int16_t>& samples) {
  auto asr = CreateAsr(config);
  if (!asr) {
    ++g_failures;
    return;
  }
  EventLog first;
  CHECK(asr->set_callback([&](const capability::TextEvent& event) {
          first.events.push_back(event);
        }).ok());
  const std::size_t cut = std::min<std::size_t>(samples.size(), 16000U);
  std::vector<std::int16_t> head(samples.begin(),
                                 samples.begin() + static_cast<std::ptrdiff_t>(cut));
  CHECK(FeedAll(*asr, head, &first, false));
  CHECK(asr->cancel().ok());
  std::size_t offset = cut;
  bool cancelled_feed = false;
  while (offset < samples.size()) {
    std::size_t consumed = 0;
    const auto frame = MakeFrame(samples, offset, &consumed);
    offset += consumed;
    const auto fed = asr->feed(frame, offset >= samples.size());
    if (fed.error.code == domain::ErrorCode::kCancelled) {
      cancelled_feed = true;
      break;
    }
    CHECK(fed.ok());
  }
  CHECK(cancelled_feed);
  for (const auto& event : first.events) {
    CHECK(event.kind != capability::TextEventKind::kFinal);
  }

  EventLog second;
  CHECK(asr->set_callback([&](const capability::TextEvent& event) {
          second.events.push_back(event);
        }).ok());
  CHECK(FeedAll(*asr, samples, &second));
  std::size_t finals = 0;
  for (const auto& event : second.events) {
    if (event.kind == capability::TextEventKind::kFinal) {
      ++finals;
    }
  }
  CHECK(finals == 1);
}

void RunCallbackFailureCase(const backend::RknnZipformerConfig& config,
                            const std::vector<std::int16_t>& samples) {
  auto asr = CreateAsr(config);
  if (!asr) {
    ++g_failures;
    return;
  }
  CHECK(asr->set_callback([](const capability::TextEvent& event) {
          if (event.kind == capability::TextEventKind::kPartial) {
            throw std::runtime_error("硬件测试故意让回调失败");
          }
        }).ok());
  bool threw = false;
  try {
    (void)FeedAll(*asr, samples, nullptr);
  } catch (const std::runtime_error& error) {
    threw = std::string(error.what()) == "硬件测试故意让回调失败";
  }
  CHECK(threw);
  std::size_t consumed = 0;
  const auto first_frame = MakeFrame(samples, 0, &consumed);
  CHECK(asr->feed(first_frame, false).error.code ==
        domain::ErrorCode::kCancelled);

  EventLog after;
  CHECK(asr->set_callback([&](const capability::TextEvent& event) {
          after.events.push_back(event);
        }).ok());
  CHECK(FeedAll(*asr, samples, &after));
  std::size_t finals = 0;
  for (const auto& event : after.events) {
    if (event.kind == capability::TextEventKind::kFinal) {
      ++finals;
    }
  }
  CHECK(finals == 1);
}

void RunInvalidCase(const backend::RknnZipformerConfig& config) {
  backend::RknnZipformerConfig invalid = config;
  invalid.encoder_model_path += ".missing";
  invalid.decoder_model_path += ".missing";
  invalid.joiner_model_path += ".missing";
  invalid.vocab_path += ".missing";
  auto created = backend::RknnZipformerAsr::create(invalid);
  CHECK(!created.ok());
  CHECK(created.error.code == domain::ErrorCode::kInvalidInput ||
        created.error.code == domain::ErrorCode::kBackendFailure);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) {
    std::cerr << "用法: " << argv[0]
              << " <encoder.rknn> <decoder.rknn> <joiner.rknn> <vocab.txt>"
                 " <test.wav> [mode] [repeat]" << std::endl;
    return 2;
  }
  const std::string mode = argc >= 7 ? argv[6] : "success";
  const int repeat = argc >= 8 ? std::max(1, std::atoi(argv[7])) : 1;
  const backend::RknnZipformerConfig config = MakeConfig(argc, argv);

  WavData wav;
  if (!ReadWav(argv[5], wav)) {
    std::cerr << "WAV 读取失败或格式不是 16 kHz/单声道/S16_LE: " << argv[5]
              << std::endl;
    return 2;
  }

  if (mode == "invalid") {
    RunInvalidCase(config);
  } else if (mode == "success") {
    RunSuccessRepeat(config, wav.samples, repeat);
  } else if (mode == "short") {
    std::vector<std::int16_t> short_audio(
        wav.samples.begin(),
        wav.samples.begin() +
            static_cast<std::ptrdiff_t>(std::min<std::size_t>(wav.samples.size(), 16000U)));
    RunSuccessCase(config, short_audio, "", false);
  } else if (mode == "long") {
    std::vector<std::int16_t> long_audio = wav.samples;
    for (int index = 0; index < 2; ++index) {
      long_audio.insert(long_audio.end(), wav.samples.begin(), wav.samples.end());
    }
    RunSuccessCase(config, long_audio, "", false);
  } else if (mode == "cancel") {
    RunCancelCase(config, wav.samples);
  } else if (mode == "callback-failure") {
    RunCallbackFailureCase(config, wav.samples);
  } else {
    std::cerr << "未知模式: " << mode << std::endl;
    return 2;
  }

  if (g_failures == 0) {
    std::cout << "rknn_zipformer_hardware_test 通过" << std::endl;
    return 0;
  }
  std::cerr << "rknn_zipformer_hardware_test 失败 " << g_failures << " 项"
            << std::endl;
  return 1;
}

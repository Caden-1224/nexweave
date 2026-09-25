// MeloTTS 合成适配器单元测试：用可控 Fake 编码器/解码器验证首帧早于完整合成、
// 固定 320 样本帧、尾部补零、取消丢弃、回调异常封锁、模型错误和资源释放。
// 测试不加载真实模型、不创建后台线程，全部行为通过公共 ITts 与统计快照观察。
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "../test_support.hpp"
#include "melotts/melotts_tts.hpp"

using namespace nexweave;

namespace {

std::string g_temp_counter = "0";

struct TempFile {
  std::string path;
  explicit TempFile(std::string file_path) : path(std::move(file_path)) {}
  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
};

TempFile WriteTempFile(const std::string& suffix, const std::string& content) {
  const std::string name = "nexweave_melotts_tts_" +
                           std::to_string(static_cast<long>(::getpid())) + "_" +
                           g_temp_counter + suffix;
  g_temp_counter = std::to_string(std::stoul(g_temp_counter) + 1U);
  const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error("无法创建测试临时文件");
  }
  output << content;
  output.close();
  return TempFile(path.string());
}

std::string TokensFixture() {
  return "_ 0\n"
         "UNK 1\n"
         "b 2\n"
         "ang 3\n"
         "g 4\n"
         "ey 5\n"
         "t 6\n"
         "w 7\n"
         ", 8\n";
}

std::string LexiconFixture() {
  return "帮 b 1\n"
         "gateway g ey t w ey 7 9 7 7 10\n"
         "， , 0\n";
}

backend::MeloTextFrontend MakeFrontend() {
  TempFile tokens = WriteTempFile(".tokens", TokensFixture());
  TempFile lexicon = WriteTempFile(".lexicon", LexiconFixture());
  auto loaded = backend::MeloTextFrontend::create(lexicon.path, tokens.path);
  CHECK(loaded.ok());
  return std::move(*loaded.value);
}

class FakeEncoder final : public backend::IMeloEncoder {
 public:
  FakeEncoder(std::size_t frames_per_phone,
              std::size_t channels,
              std::size_t samples_per_frame,
              std::shared_ptr<std::atomic<int>> destroyed = {})
      : frames_per_phone_(frames_per_phone),
        channels_(channels),
        samples_per_frame_(samples_per_frame),
        destroyed_(std::move(destroyed)) {}

  ~FakeEncoder() override {
    if (destroyed_) {
      destroyed_->fetch_sub(1);
    }
  }

  domain::OperationResult run(const backend::MeloEncoderRequest& request,
                              backend::MeloEncoderResponse& response) override {
    ++runs_;
    if (fail_run_) {
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                              "注入编码器失败");
    }
    if (request.phones == nullptr || request.tones == nullptr ||
        request.languages == nullptr || request.phone_count == 0U ||
        request.phone_count != request.tone_count ||
        request.phone_count != request.language_count) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "Fake 编码器输入不合法");
    }
    response.channels = channels_;
    response.phone_lengths.assign(request.phone_count,
                                  static_cast<std::int32_t>(frames_per_phone_));
    response.frames = request.phone_count * frames_per_phone_;
    response.z_p.assign(response.channels * response.frames, 0.0F);
    response.audio_len_samples =
        static_cast<std::int32_t>(response.frames * samples_per_frame_);
    return domain::OperationResult::success();
  }

  void set_fail_run(bool fail) { fail_run_ = fail; }
  int runs() const noexcept { return runs_; }

 private:
  std::size_t frames_per_phone_ = 0;
  std::size_t channels_ = 0;
  std::size_t samples_per_frame_ = 0;
  std::shared_ptr<std::atomic<int>> destroyed_;
  bool fail_run_ = false;
  int runs_ = 0;
};

class FakeDecoder final : public backend::IMeloDecoder {
 public:
  FakeDecoder(std::size_t channels,
              std::size_t frames_per_call,
              std::size_t samples_per_frame,
              float constant_value,
              std::shared_ptr<std::atomic<int>> destroyed = {})
      : info_{channels, frames_per_call, samples_per_frame},
        constant_value_(constant_value),
        destroyed_(std::move(destroyed)) {}

  ~FakeDecoder() override {
    if (destroyed_) {
      destroyed_->fetch_sub(1);
    }
  }

  const backend::MeloDecoderInfo& info() const noexcept override { return info_; }

  domain::OperationResult decode(const float* z_p,
                                 std::size_t total_z_frames,
                                 std::size_t frame_offset,
                                 std::size_t frame_count,
                                 std::vector<float>& audio) override {
    ++calls_;
    if (z_p == nullptr || frame_count == 0U || frame_count > info_.frames_per_call ||
        frame_offset + frame_count > total_z_frames) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "Fake 解码切片参数非法");
    }
    audio.assign(frame_count * info_.samples_per_frame, constant_value_);
    return domain::OperationResult::success();
  }

  int calls() const noexcept { return calls_; }

 private:
  backend::MeloDecoderInfo info_;
  float constant_value_ = 0.0F;
  std::shared_ptr<std::atomic<int>> destroyed_;
  int calls_ = 0;
};

struct Fixture {
  std::unique_ptr<FakeEncoder> encoder;
  std::unique_ptr<FakeDecoder> decoder;
  backend::MeloTextFrontend frontend;
  std::unique_ptr<backend::MeloTtsTts> tts;

  FakeEncoder* encoder_ptr = nullptr;
  FakeDecoder* decoder_ptr = nullptr;
};

Fixture MakeFixture(std::size_t frames_per_phone,
                    std::size_t channels,
                    std::size_t frames_per_call,
                    std::size_t samples_per_frame,
                    float constant_value,
                    std::size_t max_text_bytes = 64U * 1024U) {
  Fixture fixture;
  auto destroyed = std::make_shared<std::atomic<int>>(0);
  fixture.encoder = std::make_unique<FakeEncoder>(frames_per_phone, channels,
                                                  samples_per_frame, destroyed);
  fixture.decoder = std::make_unique<FakeDecoder>(channels, frames_per_call,
                                                  samples_per_frame, constant_value,
                                                  destroyed);
  fixture.encoder_ptr = fixture.encoder.get();
  fixture.decoder_ptr = fixture.decoder.get();
  fixture.frontend = MakeFrontend();

  backend::MeloTtsSynthesisOptions options;
  options.g = {0.0F};
  options.native_sample_rate_hz = 44100U;
  options.speed = 1.0F;
  options.noise_scale = 0.0F;
  options.noise_scale_w = 0.0F;
  options.sdp_ratio = 0.0F;
  options.max_text_bytes = max_text_bytes;
  options.max_encoder_phones = 240U;
  options.max_z_frames_per_chunk = 8192U;

  auto created = backend::MeloTtsTts::create(
      std::move(fixture.encoder), std::move(fixture.decoder),
      std::move(fixture.frontend), std::move(options));
  CHECK(created.ok());
  fixture.tts = std::move(*created.value);
  return fixture;
}

void ValidateFrame(const domain::AudioFrame& frame) {
  CHECK(frame.sample_rate_hz == domain::kAudioSampleRateHz);
  CHECK(frame.channels == domain::kAudioChannels);
  CHECK(frame.format == domain::AudioSampleFormat::kS16LE);
  CHECK(frame.samples.size() == domain::kAudioFrameSamples);
}

void TestShortTextTailAndFirstCallback() {
  Fixture fixture = MakeFixture(1U, 2U, 128U, 512U, 0.25F);
  std::vector<domain::AudioFrame> frames;
  bool synthesize_returned = false;
  bool first_callback_before_return = false;
  CHECK(fixture.tts->set_callback([&](const domain::AudioFrame& frame) {
           ValidateFrame(frame);
           if (frames.empty() && !synthesize_returned) {
             first_callback_before_return = true;
           }
           frames.push_back(frame);
         }).ok());
  CHECK(fixture.tts->synthesize("帮").ok());
  synthesize_returned = true;

  CHECK(first_callback_before_return);
  CHECK(frames.size() == 2U);
  CHECK(fixture.decoder_ptr->calls() == 1);
  for (const auto sample : frames[0].samples) {
    CHECK(sample == 8192);
  }
  const auto stats = fixture.tts->last_statistics();
  CHECK(stats.resampled_samples == 557U);
  CHECK(stats.native_samples == 1536U);
  CHECK(stats.delivered_frames == 2U);
  CHECK(stats.tail_kind == backend::MeloTtsStatistics::TailKind::kPaddedTail);
  CHECK(stats.last_frame_valid_samples == 237U);
  for (std::size_t index = 0; index < 237U; ++index) {
    CHECK(frames[1].samples[index] == 8192);
  }
  for (std::size_t index = 237U; index < domain::kAudioFrameSamples; ++index) {
    CHECK(frames[1].samples[index] == 0);
  }
}

void TestFirstFrameBeforeAllSlices() {
  Fixture fixture = MakeFixture(30U, 2U, 128U, 512U, 0.25F);
  std::vector<domain::AudioFrame> frames;
  int calls_at_first_callback = -1;
  bool synthesize_returned = false;
  CHECK(fixture.tts->set_callback([&](const domain::AudioFrame& frame) {
           ValidateFrame(frame);
           if (frames.empty()) {
             calls_at_first_callback = fixture.decoder_ptr->calls();
             CHECK(!synthesize_returned);
           }
           frames.push_back(frame);
         }).ok());
  CHECK(fixture.tts->synthesize("帮 gateway 帮 gateway 帮").ok());
  synthesize_returned = true;

  const int total_calls = fixture.decoder_ptr->calls();
  CHECK(total_calls > 1);
  CHECK(calls_at_first_callback >= 1);
  CHECK(calls_at_first_callback < total_calls);
  CHECK(!frames.empty());
  const auto stats = fixture.tts->last_statistics();
  CHECK(stats.decoder_runs == static_cast<std::size_t>(total_calls));
  CHECK(stats.delivered_frames > 0U);
}

void TestCancelDiscardsRemainingAndRecovers() {
  Fixture fixture = MakeFixture(30U, 2U, 128U, 512U, 0.25F);
  std::vector<domain::AudioFrame> frames;
  int cancelled_at = 0;
  CHECK(fixture.tts->set_callback([&](const domain::AudioFrame& frame) {
           ValidateFrame(frame);
           frames.push_back(frame);
           if (frames.size() == 2U) {
             cancelled_at = 2;
             CHECK(fixture.tts->cancel().ok());
           }
         }).ok());
  const auto canceled = fixture.tts->synthesize("帮 gateway 帮 gateway 帮");
  CHECK(canceled.error.code == domain::ErrorCode::kCancelled);
  CHECK(cancelled_at == 2);
  CHECK(frames.size() == 2U);
  const int calls_before_recovery = fixture.decoder_ptr->calls();
  CHECK(calls_before_recovery >= 1);

  std::vector<domain::AudioFrame> recovered;
  CHECK(fixture.tts->set_callback([&](const domain::AudioFrame& frame) {
           ValidateFrame(frame);
           recovered.push_back(frame);
         }).ok());
  CHECK(fixture.tts->synthesize("帮 gateway 帮 gateway 帮").ok());
  CHECK(recovered.size() > 2U);
  const auto stats = fixture.tts->last_statistics();
  CHECK(stats.delivered_frames == recovered.size());
}

void TestCallbackFailureLocksUntilReregister() {
  Fixture fixture = MakeFixture(1U, 2U, 128U, 512U, 0.25F);
  int delivered = 0;
  CHECK(fixture.tts->set_callback([&](const domain::AudioFrame& frame) {
           ValidateFrame(frame);
           ++delivered;
           if (delivered == 2) {
             throw std::runtime_error("注入回调失败");
           }
         }).ok());
  bool threw = false;
  try {
    (void)fixture.tts->synthesize("帮 gateway 帮 gateway 帮");
  } catch (const std::runtime_error& error) {
    threw = std::string(error.what()) == "注入回调失败";
  }
  CHECK(threw);
  CHECK(delivered == 2);
  CHECK(fixture.tts->synthesize("帮").error.code == domain::ErrorCode::kCancelled);

  int recovered = 0;
  CHECK(fixture.tts->set_callback([&](const domain::AudioFrame& frame) {
           ValidateFrame(frame);
           ++recovered;
         }).ok());
  CHECK(fixture.tts->synthesize("帮").ok());
  CHECK(recovered == 2);
}

void TestInvalidInputAndEngineFailure() {
  Fixture fixture = MakeFixture(1U, 2U, 128U, 512U, 0.25F);
  CHECK(fixture.tts->set_callback({}).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(fixture.tts->synthesize("帮").error.code == domain::ErrorCode::kInvalidInput);
  CHECK(fixture.tts->set_callback([](const domain::AudioFrame&) {}).ok());
  CHECK(fixture.tts->synthesize("").error.code == domain::ErrorCode::kInvalidInput);

  Fixture small = MakeFixture(1U, 2U, 128U, 512U, 0.25F, 4U);
  CHECK(small.tts->set_callback([](const domain::AudioFrame&) {}).ok());
  CHECK(small.tts->synthesize("帮 gateway").error.code == domain::ErrorCode::kInvalidInput);

  fixture.encoder_ptr->set_fail_run(true);
  const auto failed = fixture.tts->synthesize("帮");
  CHECK(failed.error.code == domain::ErrorCode::kBackendFailure);
}

void TestCreateRejectsNullEngine() {
  auto frontend = MakeFrontend();
  auto decoder = std::make_unique<FakeDecoder>(2U, 128U, 512U, 0.25F);
  backend::MeloTtsSynthesisOptions options;
  options.g = {0.0F};
  auto created = backend::MeloTtsTts::create(nullptr, std::move(decoder),
                                             std::move(frontend), std::move(options));
  CHECK(!created.ok());
  CHECK(created.error.code == domain::ErrorCode::kInvalidInput);
}

void TestResourceCleanupOnDestruction() {
  auto destroyed = std::make_shared<std::atomic<int>>(0);
  destroyed->store(2);
  {
    auto encoder = std::make_unique<FakeEncoder>(1U, 2U, 512U, destroyed);
    auto decoder = std::make_unique<FakeDecoder>(2U, 128U, 512U, 0.0F, destroyed);
    auto frontend = MakeFrontend();
    backend::MeloTtsSynthesisOptions options;
    options.g = {0.0F};
    auto created = backend::MeloTtsTts::create(std::move(encoder), std::move(decoder),
                                               std::move(frontend), std::move(options));
    CHECK(created.ok());
  }
  CHECK(destroyed->load() == 0);
}

}  // namespace

int main() {
  TestShortTextTailAndFirstCallback();
  TestFirstFrameBeforeAllSlices();
  TestCancelDiscardsRemainingAndRecovers();
  TestCallbackFailureLocksUntilReregister();
  TestInvalidInputAndEngineFailure();
  TestCreateRejectsNullEngine();
  TestResourceCleanupOnDestruction();
  return 0;
}

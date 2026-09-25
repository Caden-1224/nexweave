// Zipformer 流式适配器核心的确定性测试：用脚本化前端和引擎覆盖窗口触发、结束刷新、
// 取消、新轮重置、回调异常、容量边界与词表解析。测试不依赖 RKNN、kaldi 或音频设备。
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <unistd.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../test_support.hpp"
#include "zipformer_stream.hpp"

using namespace nexweave;

namespace {

using nexweave::backend::zipformer_detail::IFeatureFrontend;
using nexweave::backend::zipformer_detail::IZipformerEngine;
using nexweave::backend::zipformer_detail::ZipformerModelInfo;
using nexweave::backend::zipformer_detail::ZipformerStreamConfig;

ZipformerModelInfo SmallModelInfo() {
  ZipformerModelInfo info;
  info.feature_dim = 2;
  info.chunk_feature_frames = 3;
  info.encoder_output_frames = 2;
  info.encoder_subsampling_factor = 1;
  info.decoder_output_dim = 2;
  info.decoder_context_tokens = 2;
  info.joiner_output_classes = 5;
  info.feature_frame_shift_samples = 160;
  info.blank_token_id = 0;
  info.unk_token_id = 2;
  return info;
}

class FakeFrontend final : public IFeatureFrontend {
 public:
  explicit FakeFrontend(std::size_t feature_dim) : feature_dim_(feature_dim) {}

  void reset() override {
    frames_.clear();
    first_frame_index_ = 0;
    total_frames_ = 0;
    pending_.clear();
  }

  domain::OperationResult accept(const std::int16_t* samples,
                                 std::size_t count) override {
    if (samples == nullptr || count == 0) {
      return count == 0 ? domain::OperationResult::success()
                        : domain::OperationResult::failure(
                              domain::ErrorCode::kInvalidInput);
    }
    for (std::size_t index = 0; index < count; ++index) {
      pending_.push_back(samples[index]);
      if (pending_.size() == 160U) {
        PushFrame();
      }
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult append_silence(std::size_t count) override {
    if (count == 0) {
      return domain::OperationResult::success();
    }
    for (std::size_t index = 0; index < count; ++index) {
      pending_.push_back(0);
      if (pending_.size() == 160U) {
        PushFrame();
      }
    }
    return domain::OperationResult::success();
  }

  std::size_t available_feature_frames() const override { return total_frames_; }

  const float* feature_frame(std::size_t absolute_index) const override {
    if (absolute_index < first_frame_index_ ||
        absolute_index >= first_frame_index_ + frames_.size()) {
      return nullptr;
    }
    return frames_[absolute_index - first_frame_index_].data();
  }

  domain::OperationResult discard_first(std::size_t frame_count) override {
    if (frame_count > frames_.size()) {
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure, "测试前端丢弃帧数超过保留量");
    }
    frames_.erase(frames_.begin(), frames_.begin() + frame_count);
    first_frame_index_ += frame_count;
    return domain::OperationResult::success();
  }

 private:
  void PushFrame() {
    pending_.clear();
    std::vector<float> frame(feature_dim_, static_cast<float>(total_frames_));
    frames_.push_back(std::move(frame));
    ++total_frames_;
  }

  std::size_t feature_dim_ = 0;
  std::vector<std::vector<float>> frames_;
  std::size_t first_frame_index_ = 0;
  std::size_t total_frames_ = 0;
  std::vector<std::int16_t> pending_;
};

class FakeEngine final : public IZipformerEngine {
 public:
  explicit FakeEngine(ZipformerModelInfo info, std::vector<std::size_t> token_script)
      : info_(info), token_script_(std::move(token_script)) {}

  const ZipformerModelInfo& info() const noexcept override { return info_; }

  domain::OperationResult reset_round() override {
    ++reset_calls;
    if (fail_reset) {
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                              "脚本化引擎重置失败");
    }
    join_calls = 0;
    encoded_chunks.clear();
    decoded_contexts.clear();
    return domain::OperationResult::success();
  }

  domain::OperationResult encode_chunk(const float* features,
                                       std::size_t frame_count,
                                       std::vector<float>& encoder_output) override {
    ++encode_calls;
    if (fail_encode) {
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                              "脚本化引擎编码失败");
    }
    if (block_encode.load()) {
      encode_entered.store(true);
      while (!release_encode.load()) {
        std::this_thread::yield();
      }
    }
    if (features == nullptr || frame_count != info_.chunk_feature_frames) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "脚本化引擎编码参数非法");
    }
    encoded_chunks.assign(features,
                          features + frame_count * info_.feature_dim);
    encoder_output.assign(info_.encoder_output_frames * info_.decoder_output_dim,
                          0.0F);
    return domain::OperationResult::success();
  }

  domain::OperationResult decode(const std::int64_t* context,
                                 std::size_t context_size,
                                 std::vector<float>& decoder_output) override {
    ++decode_calls;
    if (fail_decode) {
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                              "脚本化引擎解码失败");
    }
    if (context == nullptr || context_size != info_.decoder_context_tokens) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "脚本化引擎解码参数非法");
    }
    decoded_contexts.emplace_back(context, context + context_size);
    decoder_output.assign(info_.decoder_output_dim, 0.0F);
    return domain::OperationResult::success();
  }

  domain::OperationResult join(const float* encoder_output,
                               const float* decoder_output,
                               std::vector<float>& logits) override {
    ++join_calls_total;
    if (fail_join) {
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                              "脚本化引擎 join 失败");
    }
    if (encoder_output == nullptr || decoder_output == nullptr) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "脚本化引擎 join 参数非法");
    }
    const std::size_t token =
        join_calls < token_script_.size() ? token_script_[join_calls] : 0U;
    ++join_calls;
    logits.assign(info_.joiner_output_classes, 0.0F);
    logits[token] = 1.0F;
    return domain::OperationResult::success();
  }

  ZipformerModelInfo info_;
  std::vector<std::size_t> token_script_;
  bool fail_reset = false;
  bool fail_encode = false;
  bool fail_decode = false;
  bool fail_join = false;
  std::size_t reset_calls = 0;
  std::size_t encode_calls = 0;
  std::size_t decode_calls = 0;
  std::size_t join_calls = 0;
  std::size_t join_calls_total = 0;
  std::atomic<bool> block_encode{false};
  std::atomic<bool> encode_entered{false};
  std::atomic<bool> release_encode{true};
  std::vector<float> encoded_chunks;
  std::vector<std::vector<std::int64_t>> decoded_contexts;
};

domain::AudioFrame MakeFrame(std::int16_t value) {
  const auto frame =
      domain::AudioFrame::from_samples(std::vector<std::int16_t>(320, value));
  CHECK(frame.ok());
  return frame.value;
}

std::vector<std::string> SmallVocabulary() {
  return {"<blk>", "<sos/eos>", "<unk>", "A", "B"};
}

struct StreamHarness {
  StreamHarness(std::size_t chunk_frames = 3, std::size_t output_frames = 2)
      : info(SmallModelInfo()) {
    info.chunk_feature_frames = chunk_frames;
    info.encoder_output_frames = output_frames;
  }

  domain::Result<std::unique_ptr<backend::ZipformerStreamAsr>> Make(
      std::vector<std::size_t> script,
      ZipformerStreamConfig config = {}) {
    auto frontend = std::make_unique<FakeFrontend>(info.feature_dim);
    auto engine = std::make_unique<FakeEngine>(info, std::move(script));
    engine_ptr = engine.get();
    return backend::ZipformerStreamAsr::create(
        std::move(frontend), std::move(engine), SmallVocabulary(), config);
  }

  ZipformerModelInfo info;
  FakeEngine* engine_ptr = nullptr;
};

// 固定合法帧持续输入必须先产生 partial，再一次 final 与 done；尾帧补齐也要被消费。
void TestPartialThenFinalAndTailFlush() {
  StreamHarness harness;
  auto created = harness.Make({3, 4});
  CHECK(created.ok());
  auto asr = std::move(*created.value);
  std::vector<capability::TextEvent> events;
  CHECK(asr->set_callback([&](const capability::TextEvent& event) {
            events.push_back(event);
          }).ok());
  CHECK(asr->feed(MakeFrame(1), false).ok());
  CHECK(events.empty());
  CHECK(asr->feed(MakeFrame(2), false).ok());
  CHECK(events.size() == 1);
  CHECK(events[0].kind == capability::TextEventKind::kPartial);
  CHECK(events[0].text == "AB");
  CHECK(asr->feed(MakeFrame(3), true).ok());
  CHECK(events.size() == 3);
  CHECK(events[1].kind == capability::TextEventKind::kFinal);
  CHECK(events[1].text == "AB");
  CHECK(events[2].kind == capability::TextEventKind::kDone);
  CHECK(events[2].text.empty());
  CHECK(harness.engine_ptr->reset_calls == 1);
  CHECK(harness.engine_ptr->encode_calls >= 2);
}

// 正常 final 后可直接开始新轮；新轮必须清空旧假设、旧特征和旧引擎缓存。
void TestNewRoundAfterFinalDoesNotLeakContext() {
  StreamHarness harness;
  auto created = harness.Make({3, 4});
  CHECK(created.ok());
  auto asr = std::move(*created.value);
  std::vector<capability::TextEvent> first;
  CHECK(asr->set_callback([&](const capability::TextEvent& event) {
            first.push_back(event);
          }).ok());
  CHECK(asr->feed(MakeFrame(1), false).ok());
  CHECK(asr->feed(MakeFrame(2), true).ok());
  CHECK(first.size() == 3);
  CHECK(first[0].text == "AB");
  CHECK(first[1].kind == capability::TextEventKind::kFinal);
  const std::size_t resets_before = harness.engine_ptr->reset_calls;

  std::vector<capability::TextEvent> second;
  CHECK(asr->set_callback([&](const capability::TextEvent& event) {
            second.push_back(event);
          }).ok());
  CHECK(asr->feed(MakeFrame(5), false).ok());
  CHECK(asr->feed(MakeFrame(6), true).ok());
  CHECK(second.size() == 3);
  CHECK(second[0].text == "AB");
  CHECK(second[1].kind == capability::TextEventKind::kFinal);
  CHECK(second[2].kind == capability::TextEventKind::kDone);
  CHECK(harness.engine_ptr->reset_calls == resets_before + 1);
}

// 取消是原子标志；返回后 feed 只回 kCancelled，不再发 partial/final/done。重新注册可恢复。
void TestCancelAndExplicitRestart() {
  StreamHarness harness;
  auto created = harness.Make({3, 4});
  CHECK(created.ok());
  auto asr = std::move(*created.value);
  std::vector<capability::TextEvent> events;
  CHECK(asr->set_callback([&](const capability::TextEvent& event) {
            events.push_back(event);
          }).ok());
  CHECK(asr->feed(MakeFrame(1), false).ok());
  CHECK(asr->cancel().ok());
  CHECK(asr->cancel().ok());
  CHECK(asr->feed(MakeFrame(2), true).error.code == domain::ErrorCode::kCancelled);
  CHECK(events.empty());
  CHECK(asr->set_callback([&](const capability::TextEvent& event) {
            events.push_back(event);
          }).ok());
  CHECK(asr->feed(MakeFrame(3), false).ok());
  CHECK(asr->feed(MakeFrame(4), true).ok());
  CHECK(events.size() == 3);
  CHECK(events[0].kind == capability::TextEventKind::kPartial);
  CHECK(events[1].kind == capability::TextEventKind::kFinal);
  CHECK(events[2].kind == capability::TextEventKind::kDone);
}

// 回调异常必须传播并封锁本轮，避免下一次 feed 重放同一段识别结果。
void TestCallbackFailureLocksRound() {
  StreamHarness harness;
  auto created = harness.Make({3, 4});
  CHECK(created.ok());
  auto asr = std::move(*created.value);
  std::vector<capability::TextEvent> events;
  CHECK(asr->set_callback([&](const capability::TextEvent& event) {
            events.push_back(event);
            if (event.kind == capability::TextEventKind::kPartial) {
              throw std::runtime_error("接收方处理 partial 失败");
            }
          }).ok());
  bool threw = false;
  try {
    CHECK(asr->feed(MakeFrame(1), false).ok());
    CHECK(asr->feed(MakeFrame(2), false).ok());
  } catch (const std::runtime_error& error) {
    threw = std::string(error.what()) == "接收方处理 partial 失败";
  }
  CHECK(threw);
  CHECK(events.size() == 1);
  CHECK(asr->feed(MakeFrame(3), true).error.code == domain::ErrorCode::kCancelled);
  CHECK(events.size() == 1);

  CHECK(asr->set_callback([&](const capability::TextEvent& event) {
            events.push_back(event);
          }).ok());
  CHECK(asr->feed(MakeFrame(4), false).ok());
  CHECK(asr->feed(MakeFrame(5), true).ok());
  CHECK(events.size() == 4);
  CHECK(events[1].kind == capability::TextEventKind::kPartial);
  CHECK(events[2].kind == capability::TextEventKind::kFinal);
  CHECK(events[3].kind == capability::TextEventKind::kDone);
}

// 非法回调/帧/容量边界返回结构化错误且不产生事件；容量超限不消费新帧。
void TestInvalidInputAndCapacityBoundary() {
  StreamHarness harness;
  auto created = harness.Make({3, 4});
  CHECK(created.ok());
  auto asr = std::move(*created.value);
  std::vector<capability::TextEvent> events;
  CHECK(asr->set_callback({}).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(asr->set_callback([&](const capability::TextEvent& event) {
            events.push_back(event);
          }).ok());

  auto invalid = MakeFrame(1);
  invalid.sample_rate_hz = 8000;
  CHECK(asr->feed(invalid, false).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(events.empty());

  ZipformerStreamConfig config;
  config.max_round_samples = 320;
  auto limited_created = harness.Make({3, 4}, config);
  CHECK(limited_created.ok());
  auto limited = std::move(*limited_created.value);
  std::vector<capability::TextEvent> limited_events;
  CHECK(limited->set_callback([&](const capability::TextEvent& event) {
            limited_events.push_back(event);
          }).ok());
  CHECK(limited->feed(MakeFrame(1), false).ok());
  CHECK(limited->feed(MakeFrame(2), false).error.code ==
        domain::ErrorCode::kBackendFailure);
  CHECK(limited_events.empty());
  CHECK(limited->set_callback([&](const capability::TextEvent& event) {
            limited_events.push_back(event);
          }).ok());
}

// 引擎推理失败要返回 kBackendFailure 并封锁本轮，不能静默补一段假文本。
void TestEngineFailureLocksRound() {
  StreamHarness harness;
  auto created = harness.Make({3, 4});
  CHECK(created.ok());
  harness.engine_ptr->fail_encode = true;
  auto asr = std::move(*created.value);
  std::vector<capability::TextEvent> events;
  CHECK(asr->set_callback([&](const capability::TextEvent& event) {
            events.push_back(event);
          }).ok());
  CHECK(asr->feed(MakeFrame(1), false).ok());
  CHECK(asr->feed(MakeFrame(2), false).error.code ==
        domain::ErrorCode::kBackendFailure);
  CHECK(events.empty());
  CHECK(asr->feed(MakeFrame(3), true).error.code == domain::ErrorCode::kCancelled);
}

// 模型几何非法时 create 必须失败，不能等到推理窗口才暴露越界。
void TestInvalidModelInfoRejected() {
  StreamHarness harness;
  harness.info.feature_dim = 0;
  auto created = harness.Make({3, 4});
  CHECK(!created.ok());
  CHECK(created.error.code == domain::ErrorCode::kInvalidInput);
}

// 并发取消的线性化点：feed 阻塞在推理中时从另一线程 cancel，返回后不得再开始回调。
// 重复 20 次，覆盖“取消置位后 encoder 返回”的固定时序；不使用 sleep，避免慢机器假失败。
void TestConcurrentCancelLinearization() {
  for (int iteration = 0; iteration < 20; ++iteration) {
    StreamHarness harness;
    auto created = harness.Make({3, 4});
    CHECK(created.ok());
    harness.engine_ptr->block_encode.store(true);
    harness.engine_ptr->release_encode.store(false);
    auto asr = std::move(*created.value);
    std::atomic<int> callbacks{0};
    CHECK(asr->set_callback([&](const capability::TextEvent&) {
              callbacks.fetch_add(1);
            }).ok());

    std::atomic<int> feed_code{static_cast<int>(domain::ErrorCode::kNone)};
    std::thread feed_thread([&]() {
      const auto first = asr->feed(MakeFrame(1), false);
      if (!first.ok()) {
        feed_code.store(static_cast<int>(first.error.code));
        return;
      }
      const auto second = asr->feed(MakeFrame(2), false);
      feed_code.store(static_cast<int>(second.error.code));
    });

    for (int spin = 0; spin < 20000 &&
                      !harness.engine_ptr->encode_entered.load(); ++spin) {
      std::this_thread::yield();
    }
    const bool entered = harness.engine_ptr->encode_entered.load();
    CHECK(entered);
    if (entered) {
      CHECK(asr->cancel().ok());
    }
    harness.engine_ptr->release_encode.store(true);
    feed_thread.join();
    if (entered) {
      CHECK(feed_code.load() == static_cast<int>(domain::ErrorCode::kCancelled));
    }
    CHECK(callbacks.load() == 0);
  }
}

// 词表解析：合法文件按 id 索引，缺文件、重复 id、非整数 id 都返回结构化失败。
void TestVocabularyLoader() {
  const std::string path =
      "/tmp/nexweave_zipformer_vocab_" + std::to_string(::getpid()) + ".txt";
  {
    std::ofstream output(path, std::ios::binary);
    output << "<blk> 0\nA 1\n<unk> 2\nB 3\n";
  }
  auto loaded = backend::zipformer_detail::load_zipformer_vocabulary(path);
  CHECK(loaded.ok());
  CHECK(loaded.value->size() == 4);
  CHECK((*loaded.value)[0] == "<blk>");
  CHECK((*loaded.value)[1] == "A");
  CHECK((*loaded.value)[3] == "B");

  auto missing =
      backend::zipformer_detail::load_zipformer_vocabulary(path + ".missing");
  CHECK(!missing.ok());
  CHECK(missing.error.code == domain::ErrorCode::kInvalidInput);

  {
    std::ofstream output(path, std::ios::binary);
    output << "A 1\nB 1\n";
  }
  auto duplicate = backend::zipformer_detail::load_zipformer_vocabulary(path);
  CHECK(!duplicate.ok());

  {
    std::ofstream output(path, std::ios::binary);
    output << "A abc\n";
  }
  auto malformed = backend::zipformer_detail::load_zipformer_vocabulary(path);
  CHECK(!malformed.ok());

  std::remove(path.c_str());
}

}  // namespace

int main() {
  TestPartialThenFinalAndTailFlush();
  TestNewRoundAfterFinalDoesNotLeakContext();
  TestCancelAndExplicitRestart();
  TestCallbackFailureLocksRound();
  TestInvalidInputAndCapacityBoundary();
  TestEngineFailureLocksRound();
  TestInvalidModelInfoRejected();
  TestConcurrentCancelLinearization();
  TestVocabularyLoader();
  return 0;
}

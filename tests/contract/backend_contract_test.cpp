// 能力契约夹具只验证六种接口的最小外部行为，不冒充完整 Fake 模型或设备实现。
// 同步回调和内存样本由夹具拥有；取消后不再提交事件，不创建线程、设备或文件。
#include "../test_support.hpp"
#include "backend.hpp"

using namespace nexweave;
namespace {
class Fixture final : public capability::IAsr,
                      public capability::IRag,
                      public capability::ILlm,
                      public capability::ITts,
                      public capability::IAudioSource,
                      public capability::IAudioSink {
 public:
  domain::OperationResult set_callback(capability::TextEventCallback callback) override {
    if (!callback) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    text_callback_ = std::move(callback);
    cancelled_ = false;
    return domain::OperationResult::success();
  }
  domain::OperationResult set_callback(capability::AudioEventCallback callback) override {
    if (!callback) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    audio_callback_ = std::move(callback);
    cancelled_ = false;
    return domain::OperationResult::success();
  }
  domain::OperationResult feed(const domain::AudioFrame& frame, bool is_last) override {
    if (cancelled_) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
    }
    if (!text_callback_ || !domain::validate_audio_frame(frame).ok()) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    text_callback_(
        {is_last ? capability::TextEventKind::kFinal : capability::TextEventKind::kPartial,
         "heard",
         {}});
    return domain::OperationResult::success();
  }
  domain::Result<std::vector<capability::RetrievedChunk>> retrieve(const std::string&,
                                                                   std::size_t top_k) override {
    std::vector<capability::RetrievedChunk> chunks;
    if (top_k != 0) {
      chunks.push_back({"id", "text", 1.0});
    }
    return domain::Result<std::vector<capability::RetrievedChunk>>::success(std::move(chunks));
  }
  domain::OperationResult generate(const std::string& prompt) override {
    if (cancelled_) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
    }
    if (prompt.empty() || !text_callback_) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    text_callback_({capability::TextEventKind::kToken, "answer", {}});
    text_callback_({capability::TextEventKind::kDone, "", {}});
    return domain::OperationResult::success();
  }
  domain::OperationResult synthesize(const std::string& text) override {
    if (cancelled_) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
    }
    if (text.empty() || !audio_callback_) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
    }
    const auto frame = domain::AudioFrame::from_samples(std::vector<std::int16_t>(320));
    audio_callback_(frame.value);
    return domain::OperationResult::success();
  }
  domain::OperationResult cancel() noexcept override {
    cancelled_ = true;
    return domain::OperationResult::success();
  }
  domain::OperationResult open() override {
    opened_ = true;
    cancelled_ = false;
    return domain::OperationResult::success();
  }
  domain::Result<domain::AudioFrame> read() override {
    return domain::Result<domain::AudioFrame>::failure(domain::ErrorCode::kDeviceFailure);
  }
  domain::OperationResult write(const domain::AudioFrame& frame) override {
    if (!opened_) {
      return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure);
    }
    if (cancelled_) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
    }
    return domain::validate_audio_frame(frame).ok()
               ? domain::OperationResult::success()
               : domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }
  domain::OperationResult close() noexcept override {
    opened_ = false;
    return domain::OperationResult::success();
  }

 private:
  bool opened_ = false;
  bool cancelled_ = false;
  capability::TextEventCallback text_callback_;
  capability::AudioEventCallback audio_callback_;
};

void TestMinimalCapabilities() {
  Fixture fixture;
  // 检索成功含值，top_k=0 的空列表也是成功；空文本、未打开写入与设备失败必须可区分。
  const auto empty = fixture.retrieve("q", 0);
  CHECK(empty.ok());
  CHECK(empty.value->empty());
  const auto hit = fixture.retrieve("q", 1);
  CHECK(hit.ok());
  CHECK(hit.value->size() == 1);
  CHECK(!fixture.synthesize("").ok());
  CHECK(!fixture.read().ok());
  CHECK(!fixture.write(domain::AudioFrame{}).ok());

  // LLM 必须 token 后 done；取消后的新提交不产生任何回调，重复取消保持静默。
  capability::ILlm& llm = fixture;
  std::vector<capability::TextEventKind> kinds;
  CHECK(llm.set_callback([&](const capability::TextEvent& event) { kinds.push_back(event.kind); })
            .ok());
  CHECK(!llm.generate("").ok());
  CHECK(llm.generate("question").ok());
  CHECK(kinds.size() == 2);
  CHECK(kinds[0] == capability::TextEventKind::kToken);
  CHECK(kinds[1] == capability::TextEventKind::kDone);
  CHECK(llm.cancel().ok());
  CHECK(llm.cancel().ok());
  CHECK(llm.generate("question").error.code == domain::ErrorCode::kCancelled);
  CHECK(kinds.size() == 2);

  // ASR 只接受合法帧，结束帧生成 final；TTS 返回前交付完整 PCM，取消后不再回调。
  capability::IAsr& asr = fixture;
  kinds.clear();
  CHECK(asr.set_callback([&](const capability::TextEvent& event) { kinds.push_back(event.kind); })
            .ok());
  CHECK(!asr.feed(domain::AudioFrame{}, true).ok());
  const auto frame = domain::AudioFrame::from_samples(std::vector<std::int16_t>(320));
  CHECK(asr.feed(frame.value, true).ok());
  CHECK(kinds.size() == 1 && kinds.front() == capability::TextEventKind::kFinal);
  capability::ITts& tts = fixture;
  int audio_callbacks = 0;
  CHECK(tts.set_callback([&](const domain::AudioFrame& audio) {
             CHECK(domain::validate_audio_frame(audio).ok());
             ++audio_callbacks;
           })
            .ok());
  CHECK(tts.synthesize("text").ok());
  CHECK(audio_callbacks == 1);
  CHECK(tts.cancel().ok());
  CHECK(tts.synthesize("text").error.code == domain::ErrorCode::kCancelled);
  CHECK(audio_callbacks == 1);
}
}  // namespace

int main() {
  TestMinimalCapabilities();
}

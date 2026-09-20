// 确定性 Fake Session 夹具的共享组装入口。
//
// 职责与范围
// ----------
// 本文件把“Fake ASR/RAG/LLM/TTS + 常驻音频输入 + SessionApp”组装成同一份测试夹具，
// 供独立 Session 子进程夹具与单/多进程对照测试共同使用。它只存在于测试支持层，不是产品
// 后端工厂，也不改变任何领域契约；这样做的目的只是让两条执行拓扑使用完全相同的脚本、
// 输入、队列容量和播放策略，避免“对照实验”因为两个夹具各自漂移而失去意义。
//
// 资源与所有权
// ------------
// FakeSessionHarness 拥有全部能力对象、音频汇、播放组件、队列音频源、常驻输入和 SessionApp。
// 成员按依赖顺序声明并通过 unique_ptr 持有，析构时反向释放，因此 SessionApp 先于它借用的
// 能力对象析构。调用方只借用 app()/source() 的引用，不得让它们活过本对象。
//
// 线程与生命周期
// --------------
// 构造在调用线程完成，会打开 FakeAudioSink；失败时抛出 std::runtime_error，由调用方转成
// 明确的进程退出。SessionApp::run() 只能在模拟常驻模式下运行一次，因此每个 profile 轮次都
// 应新建一个 FakeSessionHarness，而不是复用上一轮的常驻输入状态。
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "fake_asr.hpp"
#include "fake_audio.hpp"
#include "fake_llm.hpp"
#include "fake_rag.hpp"
#include "fake_tts.hpp"
#include "queued_audio_source.hpp"
#include "resident_audio_input.hpp"
#include "session_app.hpp"

namespace nexweave::test {

class FakeSessionHarness final {
 public:
  FakeSessionHarness() {
    asr_ = std::make_unique<backend::FakeAsr>(
        std::vector<std::string>{"远端会话回答"});
    rag_ = std::make_unique<backend::FakeRag>(
        std::vector<capability::RetrievedChunk>{});
    router_ = std::make_unique<backend::FakeRagRouter>(*rag_);
    tts_ = std::make_unique<backend::FakeTts>();
    llm_ = std::make_unique<backend::FakeLlm>(
        std::vector<std::string>{"远端", "会话", "回答"});
    clock_ = std::make_unique<TickingClock>();
    sink_ = std::make_unique<backend::FakeAudioSink>();
    const domain::OperationResult opened = sink_->open();
    if (!opened.ok()) {
      throw std::runtime_error("FakeAudioSink 打开失败: " + opened.error.message);
    }
    playback_ = std::make_unique<runtime::LogicalClockPlayback>(*sink_, *clock_);

    runtime::QueuedAudioSourceConfig source_config;
    source_config.max_pending_frames = 64;
    source_ = std::make_unique<runtime::QueuedAudioSource>(source_config);

    runtime::ResidentAudioInputConfig resident_config;
    resident_config.segmentation.preroll_frames = 0;
    resident_config.segmentation.min_speech_frames = 1;
    resident_config.segmentation.min_silence_frames = 2;
    resident_config.segmentation.max_speech_frames = 1000;
    detector_ = std::make_unique<AmplitudeDetector>();
    resident_ = std::make_unique<runtime::ResidentAudioInput>(
        *source_, *detector_, "remote-session", resident_config);

    runtime::SessionAppConfig app_config;
    app_config.mode = runtime::SessionAppInputMode::kSimulatedResident;
    app_config.stream_id = "remote-session";
    app_config.resident_config = resident_config;
    app_config.session_config.playback_queue_capacity = 256;
    app_config.pump_budget_per_take = 4096;
    app_ = std::make_unique<runtime::SessionApp>(
        app_config, *asr_, *rag_, *router_, *tts_, *playback_, resident_.get(), llm_.get());
  }

  ~FakeSessionHarness() = default;

  FakeSessionHarness(const FakeSessionHarness&) = delete;
  FakeSessionHarness& operator=(const FakeSessionHarness&) = delete;

  runtime::SessionApp& app() noexcept {
    return *app_;
  }

  runtime::QueuedAudioSource& source() noexcept {
    return *source_;
  }

  std::size_t rendered_frames() const {
    return sink_->frames().size();
  }

  std::vector<runtime::ActivityMarker> trace() const {
    return app_->trace();
  }

 private:
  class TickingClock final : public runtime::IPlaybackClock {
   public:
    std::int64_t now_ms() const noexcept override {
      const_cast<TickingClock*>(this)->now_ms_ +=
          static_cast<std::int64_t>(domain::kAudioFrameDurationMs);
      return now_ms_;
    }

    void advance(std::int64_t delta_ms) override {
      if (delta_ms > 0) {
        now_ms_ += delta_ms;
      }
    }

   private:
    std::int64_t now_ms_ = 0;
  };

  class AmplitudeDetector final : public runtime::ISpeechActivityDetector {
   public:
    domain::Result<runtime::SpeechActivity> detect(
        const domain::AudioFrame& frame) override {
      for (const std::int16_t sample : frame.samples) {
        if (sample != 0) {
          return domain::Result<runtime::SpeechActivity>::success(
              runtime::SpeechActivity::kSpeech);
        }
      }
      return domain::Result<runtime::SpeechActivity>::success(
          runtime::SpeechActivity::kSilence);
    }

    void reset() override {}
  };

  // 声明顺序即依赖顺序；析构时反向执行，保证 app_ 先释放，能力对象随后释放。
  std::unique_ptr<backend::FakeAsr> asr_;
  std::unique_ptr<backend::FakeRag> rag_;
  std::unique_ptr<backend::FakeRagRouter> router_;
  std::unique_ptr<backend::FakeTts> tts_;
  std::unique_ptr<backend::FakeLlm> llm_;
  std::unique_ptr<TickingClock> clock_;
  std::unique_ptr<backend::FakeAudioSink> sink_;
  std::unique_ptr<runtime::LogicalClockPlayback> playback_;
  std::unique_ptr<runtime::QueuedAudioSource> source_;
  std::unique_ptr<AmplitudeDetector> detector_;
  std::unique_ptr<runtime::ResidentAudioInput> resident_;
  std::unique_ptr<runtime::SessionApp> app_;
};

}  // namespace nexweave::test

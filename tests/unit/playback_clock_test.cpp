#include "../test_support.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "fake_audio.hpp"
#include "session_runtime.hpp"

using namespace nexweave;
using nexweave::runtime::LogicalClockPlayback;
using nexweave::runtime::ManualPlaybackClock;

namespace {

constexpr std::size_t kFrameSamples = domain::kAudioFrameSamples;

domain::AudioFrame MakeFrame(std::int16_t value) {
  std::vector<std::int16_t> samples(kFrameSamples, value);
  const auto frame = domain::AudioFrame::from_samples(samples);
  CHECK(frame.ok());
  return frame.value;
}

// 在第 fail_at 次写入后开始返回设备错误的音频汇：用于验证播放夹具把失败原样
// 上报并停止继续写设备，而不是把部分输出当成成功。
class FailingSink final : public capability::IAudioSink {
 public:
  explicit FailingSink(std::size_t fail_at) : fail_at_(fail_at) {}

  domain::OperationResult open() override {
    return domain::OperationResult::success();
  }

  domain::OperationResult write(const domain::AudioFrame& frame) override {
    if (writes_ >= fail_at_) {
      return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure, "设备写入失败");
    }
    ++writes_;
    samples_.insert(samples_.end(), frame.samples.begin(), frame.samples.end());
    return domain::OperationResult::success();
  }

  domain::OperationResult cancel() noexcept override {
    return domain::OperationResult::success();
  }

  domain::OperationResult close() noexcept override {
    return domain::OperationResult::success();
  }

  std::size_t writes() const noexcept {
    return writes_;
  }

  std::size_t sample_count() const noexcept {
    return samples_.size();
  }

 private:
  std::size_t fail_at_ = 0;
  std::size_t writes_ = 0;
  std::vector<std::int16_t> samples_;
};

// 交付与播放进度不变量：交付的帧立即写入设备（不存在“没人推进时间就写不进去”的
// 死锁），但“已播完”的数量只由逻辑时钟决定；设备时间不前进时帧已写出却仍未播完。
// 判据是 played_count() 与 pending_count()：两者相等表示已写入设备的音频都播完了。
void TestDeliveryIsImmediateAndProgressFollowsClock() {
  backend::FakeAudioSink sink;
  CHECK(sink.open().ok());
  ManualPlaybackClock clock;
  LogicalClockPlayback playback(sink, clock);
  CHECK(playback.start().ok());
  CHECK(playback.played_count() == 0);
  CHECK(playback.pending_count() == 0);

  const auto first = MakeFrame(100);
  const auto second = MakeFrame(200);

  // 逻辑时间为 0：两帧都立即写出，但一帧都还没播完。
  CHECK(playback.render(first).ok());
  CHECK(playback.render(second).ok());
  CHECK(sink.frames().size() == 2);
  CHECK(playback.played_count() == 0);
  CHECK(playback.pending_count() == 2);
  CHECK(playback.played_frames().size() == 2);
  CHECK(playback.played_frames().front().samples.front() == 100);
  CHECK(playback.played_frames().back().samples.front() == 200);

  // 推进到 20 ms 并轮询：第一帧算作播完，第二帧的门槛是 40 ms。
  clock.advance(domain::kAudioFrameDurationMs);
  playback.poll();
  CHECK(playback.played_count() == 1);
  CHECK(playback.pending_count() == 1);
  // 轮询不写设备：设备内容不因轮询而增加。
  CHECK(sink.frames().size() == 2);

  // 推进到 40 ms 并轮询：第二帧播完，没有未播完的帧。
  clock.advance(domain::kAudioFrameDurationMs);
  playback.poll();
  CHECK(playback.played_count() == 2);
  CHECK(playback.pending_count() == 0);
  CHECK(sink.pcm_samples().size() == 2 * kFrameSamples);
}

// 停止不变量：stop() 丢弃尚未播完的部分，保留已经写入设备的声音（不可撤回），
// 并且幂等；停止后不再接受任何新帧，因此等待中的音频不会在取消后继续发声。
// stop() 同时释放本轮，使同一播放组件可以被常驻会话的下一轮重新 start()。
void TestStopKeepsWrittenFramesAndReleasesRound() {
  backend::FakeAudioSink sink;
  CHECK(sink.open().ok());
  ManualPlaybackClock clock;
  LogicalClockPlayback playback(sink, clock);
  CHECK(playback.start().ok());

  const auto first = MakeFrame(7);
  const auto second = MakeFrame(9);
  CHECK(playback.render(first).ok());
  CHECK(playback.render(second).ok());
  CHECK(sink.frames().size() == 2);
  CHECK(playback.played_count() == 0);
  CHECK(playback.pending_count() == 2);

  CHECK(playback.stop().ok());
  CHECK(playback.stop().ok());
  CHECK(playback.pending_count() == 0);
  CHECK(playback.played_count() == 0);
  // 已写入设备的两帧不可撤回，仍保留在快照中。
  CHECK(sink.frames().size() == 2);
  CHECK(playback.played_frames().size() == 2);

  // 停止后交付新帧必须失败，且不改变已写出的设备内容。
  const auto rejected = playback.render(MakeFrame(11));
  CHECK(!rejected.ok());
  CHECK(rejected.error.code == domain::ErrorCode::kAlreadyCompleted);
  CHECK(sink.frames().size() == 2);

  // 停止后可以开始新一轮：这是常驻会话跨轮次复用同一播放组件的前提。
  CHECK(playback.start().ok());
  CHECK(!playback.start().ok());
  CHECK(playback.pending_count() == 0);
  CHECK(playback.played_count() == 0);
  clock.advance(domain::kAudioFrameDurationMs);
  CHECK(playback.render(first).ok());
  playback.poll();
  CHECK(playback.played_count() == 1);
  CHECK(playback.pending_count() == 0);
}

// 状态前置条件不变量：未开始时不能写入设备；重复开始返回 kAlreadyCompleted，
// 且不会清空本轮已经写出的结果；非法帧在任何状态下都返回 kInvalidInput。
void TestStartAndFrameValidationPreconditions() {
  backend::FakeAudioSink sink;
  CHECK(sink.open().ok());
  ManualPlaybackClock clock;
  LogicalClockPlayback playback(sink, clock);

  const auto frame = MakeFrame(3);
  const auto before_start = playback.render(frame);
  CHECK(!before_start.ok());
  CHECK(before_start.error.code == domain::ErrorCode::kAlreadyCompleted);

  CHECK(playback.start().ok());
  CHECK(!playback.start().ok());
  CHECK(playback.played_count() == 0);
  CHECK(sink.frames().empty());

  domain::AudioFrame short_frame;
  short_frame.samples = std::vector<std::int16_t>(kFrameSamples - 1, 1);
  const auto invalid = playback.render(short_frame);
  CHECK(!invalid.ok());
  CHECK(invalid.error.code == domain::ErrorCode::kInvalidInput);
  CHECK(sink.frames().empty());

  CHECK(playback.render(frame).ok());
  CHECK(sink.frames().size() == 1);
  CHECK(playback.pending_count() == 1);
  clock.advance(domain::kAudioFrameDurationMs);
  playback.poll();
  CHECK(playback.played_count() == 1);
  CHECK(playback.pending_count() == 0);
}

// 设备失败不变量：写入失败必须被原样上报（错误码不丢失），失败后不再继续写设备，
// 已经写出的帧保留；本轮不会因为后面还有帧就假装成功。
void TestDeviceFailureIsReportedOnce() {
  FailingSink sink(1);
  CHECK(sink.open().ok());
  ManualPlaybackClock clock;
  LogicalClockPlayback playback(sink, clock);
  CHECK(playback.start().ok());

  CHECK(playback.render(MakeFrame(1)).ok());
  const auto failed = playback.render(MakeFrame(2));
  CHECK(!failed.ok());
  CHECK(failed.error.code == domain::ErrorCode::kDeviceFailure);
  CHECK(sink.writes() == 1);
  CHECK(sink.sample_count() == kFrameSamples);
  CHECK(playback.played_frames().size() == 1);

  const auto error = playback.error();
  CHECK(!error.ok());
  CHECK(error.error.code == domain::ErrorCode::kDeviceFailure);

  const auto after_failure = playback.render(MakeFrame(3));
  CHECK(!after_failure.ok());
  CHECK(after_failure.error.code == domain::ErrorCode::kDeviceFailure);
  CHECK(sink.writes() == 1);
}

// 停止标志借用：Session 置位的原子标志为真后，播放夹具不得再写出新帧，即使设备
// 仍有空间；这保证“受理停止”与“停止发声”之间没有隐藏的延迟写出。
void TestCancelledFlagBlocksNewFrames() {
  backend::FakeAudioSink sink;
  CHECK(sink.open().ok());
  ManualPlaybackClock clock;
  LogicalClockPlayback playback(sink, clock);
  std::atomic<bool> cancelled{false};
  playback.set_cancelled_flag(&cancelled);
  CHECK(playback.start().ok());

  const auto frame = MakeFrame(5);
  CHECK(playback.render(frame).ok());
  CHECK(sink.frames().size() == 1);

  cancelled.store(true);
  const auto blocked = playback.render(frame);
  CHECK(!blocked.ok());
  CHECK(blocked.error.code == domain::ErrorCode::kCancelled);
  CHECK(sink.frames().size() == 1);
  CHECK(playback.played_frames().size() == 1);

  // 解除借用后仍是“已经开始”的同一轮：重复开始返回 kAlreadyCompleted，
  // 说明停止标志不会改写轮次边界，新轮次必须显式 stop() 后再 start()。
  playback.set_cancelled_flag(nullptr);
  CHECK(!playback.start().ok());
  CHECK(playback.stop().ok());
  CHECK(playback.start().ok());
}

// 时钟不变量：逻辑时间只能单调不减，非正增量被忽略；帧时长非正值回退到统一的
// 20 ms 帧时长，避免出现零时长播放让每帧瞬间“播完”。
void TestClockMonotonicAndFrameDurationFallback() {
  ManualPlaybackClock clock(100);
  CHECK(clock.now_ms() == 100);
  clock.advance(20);
  CHECK(clock.now_ms() == 120);
  clock.advance(0);
  clock.advance(-5);
  CHECK(clock.now_ms() == 120);

  backend::FakeAudioSink sink;
  CHECK(sink.open().ok());
  ManualPlaybackClock zero_clock;
  LogicalClockPlayback playback(sink, zero_clock, 0);
  CHECK(playback.start().ok());
  const auto frame = MakeFrame(4);
  CHECK(playback.render(frame).ok());
  CHECK(playback.played_count() == 0);
  zero_clock.advance(domain::kAudioFrameDurationMs);
  playback.poll();
  CHECK(playback.played_count() == 1);
  CHECK(playback.pending_count() == 0);
}

}  // namespace

int main() {
  TestDeliveryIsImmediateAndProgressFollowsClock();
  TestStopKeepsWrittenFramesAndReleasesRound();
  TestStartAndFrameValidationPreconditions();
  TestDeviceFailureIsReportedOnce();
  TestCancelledFlagBlocksNewFrames();
  TestClockMonotonicAndFrameDurationFallback();
  return 0;
}

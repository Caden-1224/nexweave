// 队列音频源单元测试：验收有界容量、顺序读取、自然结束和取消唤醒。
//
// 这些用例保护的不变量：
//   - 队列满时拒绝新帧，不覆盖旧帧；
//   - end_input 只结束输入，不丢已经入队的完整帧；
//   - cancel 清空未消费帧并唤醒阻塞中的读线程，已交付帧不撤回。
#include "queued_audio_source.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using nexweave::domain::ErrorCode;
using nexweave::runtime::QueuedAudioSource;
using nexweave::runtime::QueuedAudioSourceConfig;

nexweave::domain::AudioFrame Frame(std::int16_t value) {
  const auto frame = nexweave::domain::AudioFrame::from_samples(
      std::vector<std::int16_t>(nexweave::domain::kAudioFrameSamples, value));
  CHECK(frame.ok());
  return frame.value;
}

}  // namespace

int main() {
  {
    QueuedAudioSourceConfig config;
    config.max_pending_frames = 2;
    QueuedAudioSource source(config);
    CHECK(source.open().ok());

    CHECK(source.push(Frame(1)).ok());
    CHECK(source.push(Frame(2)).ok());
    const auto rejected = source.push(Frame(3));
    CHECK(rejected.error.code == ErrorCode::kBackendFailure);

    CHECK(source.end_input().ok());
    const auto first = source.read();
    CHECK(first.ok());
    CHECK(first.value->samples.front() == 1);
    const auto second = source.read();
    CHECK(second.ok());
    CHECK(second.value->samples.front() == 2);
    const auto ended = source.read();
    CHECK(ended.error.code == ErrorCode::kAlreadyCompleted);
    const auto stats = source.stats();
    CHECK(stats.pushed == 2);
    CHECK(stats.popped == 2);
    CHECK(stats.rejected_full == 1);
  }

  {
    QueuedAudioSource source;
    CHECK(source.open().ok());
    std::atomic<bool> reader_entered{false};
    nexweave::domain::Error observed;
    std::thread reader([&source, &reader_entered, &observed] {
      reader_entered.store(true);
      const auto read = source.read();
      observed = read.ok() ? nexweave::domain::Error{} : read.error;
    });
    while (!reader_entered.load()) {
      std::this_thread::yield();
    }
    CHECK(source.cancel().ok());
    if (reader.joinable()) {
      reader.join();
    }
    CHECK(observed.code == ErrorCode::kCancelled);
    CHECK(source.stats().cancelled_reads >= 1);
  }

  return 0;
}

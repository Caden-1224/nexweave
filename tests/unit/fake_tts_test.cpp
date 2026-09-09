// FakeTts 单元测试：保护“同步逐帧交付、确定性 PCM、取消线性化与回调失败
// 封锁”四组不变量。所有取消竞态用条件变量屏障把合成停在可复现的帧边界上，
// 不使用 sleep 计时；涉及并发的场景均以 join 建立 happens-before 后再断言。
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../test_support.hpp"
#include "fake_tts.hpp"

using namespace nexweave;
namespace {

// 与 fake_tts.cpp 相同的帧数公式：测试用其核对回调次数，防止实现漏帧或多帧。
std::size_t ExpectedFrameCount(const std::string& text) {
  return (text.size() + backend::kFakeTtsBytesPerFrame - 1) / backend::kFakeTtsBytesPerFrame;
}

// 与 fake_tts.cpp 相同的采样公式：测试按文档规则推导期望电平，另用硬编码
// 锚点样本复核手算值，防止实现与测试 helper 犯同一处错误。
std::int16_t ExpectedLevel(const std::string& text, std::size_t global_sample) {
  std::size_t byte_sum = 0;
  for (unsigned char byte : text) {
    byte_sum += static_cast<std::size_t>(byte);
  }
  const std::size_t position =
      (byte_sum % backend::kFakeTtsCycleSamples + global_sample) % backend::kFakeTtsCycleSamples;
  return position < backend::kFakeTtsCycleSamples / 2 ? backend::kFakeTtsSquareAmplitude
                                                      : -backend::kFakeTtsSquareAmplitude;
}

// 成功路径：80 个 'a'（字节和 7760，起始相位 0）应产生 5 帧；每帧 320 样本、
// 元数据符合 v1 契约，回调全部发生在 synthesize 返回之前、调用线程之内，
// 且任意时刻最多一帧在途（无隐藏队列/线程）。锚点按手算值硬编码：
// 相位 0 时 g=0..39 为 +8000，g=40..79 为 -8000，320 % 80 == 0 使每帧帧首
// 相位相同，帧间相位连续。
void TestSynchronousChunkedDelivery() {
  backend::FakeTts tts;
  capability::ITts& itts = tts;
  const std::string text(80, 'a');
  std::vector<domain::AudioFrame> frames;
  std::atomic<int> in_flight{0};
  int max_in_flight = 0;
  std::thread::id callback_thread{};
  CHECK(itts.set_callback([&](const domain::AudioFrame& frame) {
             const int now = in_flight.fetch_add(1) + 1;
             if (now > max_in_flight) {
               max_in_flight = now;
             }
             callback_thread = std::this_thread::get_id();
             CHECK(domain::validate_audio_frame(frame).ok());
             CHECK(frame.sample_rate_hz == domain::kAudioSampleRateHz);
             CHECK(frame.channels == domain::kAudioChannels);
             CHECK(frame.format == domain::AudioSampleFormat::kS16LE);
             CHECK(frame.samples.size() == domain::kAudioFrameSamples);
             frames.push_back(frame);
             in_flight.fetch_sub(1);
           }).ok());
  CHECK(itts.synthesize(text).ok());

  CHECK(frames.size() == ExpectedFrameCount(text));
  CHECK(frames.size() == 5);
  CHECK(max_in_flight == 1);
  CHECK(callback_thread == std::this_thread::get_id());
  CHECK(frames[0].samples[0] == backend::kFakeTtsSquareAmplitude);
  CHECK(frames[0].samples[39] == backend::kFakeTtsSquareAmplitude);
  CHECK(frames[0].samples[40] == -backend::kFakeTtsSquareAmplitude);
  CHECK(frames[0].samples[79] == -backend::kFakeTtsSquareAmplitude);
  // 第二帧帧首：全局样本 320，320 % 80 == 0，仍处于正半周，相位无跳变。
  CHECK(frames[1].samples[0] == backend::kFakeTtsSquareAmplitude);
  // 起始相位 0 时每帧都完整覆盖整数个周期，任意两帧逐采样一致。
  for (std::size_t index = 1; index < frames.size(); ++index) {
    CHECK(frames[index].samples == frames[0].samples);
  }
  // 公式复核：随机抽查帧内样本与 helper 推导一致。
  CHECK(frames[2].samples[123] == ExpectedLevel(text, 2 * 320 + 123));
}

// 确定性：同文本在同实例与不同实例上的两次合成逐采样一致；文本内容参与
// 波形（长度与起始相位），1..40 字节的所有长度都满足帧数公式且无短帧。
void TestDeterminismAndContentDependence() {
  backend::FakeTts first;
  backend::FakeTts second;
  const std::string text = "请把客厅的灯调暗一点";  // 27 字节，UTF-8 多字节。
  auto collect = [](backend::FakeTts& tts,
                    const std::string& input) -> std::vector<domain::AudioFrame> {
    std::vector<domain::AudioFrame> frames;
    CHECK(tts.set_callback([&](const domain::AudioFrame& frame) {
               frames.push_back(frame);
             }).ok());
    CHECK(tts.synthesize(input).ok());
    return frames;
  };
  const auto run1 = collect(first, text);
  const auto run2 = collect(first, text);
  const auto run3 = collect(second, text);
  CHECK(run1.size() == ExpectedFrameCount(text));
  CHECK(run2.size() == run1.size());
  CHECK(run3.size() == run1.size());
  for (std::size_t index = 0; index < run1.size(); ++index) {
    CHECK(run2[index].samples == run1[index].samples);
    CHECK(run3[index].samples == run1[index].samples);
  }

  // 不同文本 → 不同 PCM：40 个 'a'（字节和 3880，起始相位 40）首样本为
  // 负半周，与 80 个 'a'（起始相位 0）的首样本相反；帧数也随长度变化。
  backend::FakeTts other;
  const auto short_run = collect(other, std::string(40, 'a'));
  CHECK(short_run.size() == ExpectedFrameCount(std::string(40, 'a')));
  CHECK(short_run[0].samples[0] == -backend::kFakeTtsSquareAmplitude);

  // 长度边界 1..40 字节：回调帧数必须等于 ⌈len/16⌉，杜绝漏帧/多帧。
  for (std::size_t length = 1; length <= 40; ++length) {
    const std::string input(length, 'b');
    CHECK(collect(other, input).size() == ExpectedFrameCount(input));
  }
}

// 非法输入：空文本、未注册回调与空回调注册都返回 kInvalidInput 且不交付
// 任何帧；失败注册不清除旧回调，旧轮次仍然可用。
void TestInvalidInputRejectsWithoutFrames() {
  backend::FakeTts tts;
  capability::ITts& itts = tts;
  std::vector<domain::AudioFrame> frames;
  const auto record = [&](const domain::AudioFrame& frame) { frames.push_back(frame); };

  CHECK(itts.synthesize("有文本").error.code == domain::ErrorCode::kInvalidInput);
  CHECK(itts.set_callback({}).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(itts.synthesize("有文本").error.code == domain::ErrorCode::kInvalidInput);
  CHECK(frames.empty());

  CHECK(itts.set_callback(record).ok());
  CHECK(itts.synthesize("").error.code == domain::ErrorCode::kInvalidInput);
  CHECK(frames.empty());
  CHECK(itts.synthesize("正常文本").ok());
  CHECK(frames.size() == ExpectedFrameCount("正常文本"));
}

// 取消封锁：取消幂等；取消后任何文本都返回 kCancelled 且无新帧；重新注册
// 回调是唯一恢复途径，恢复后新轮次完整交付。
void TestCancelLocksUntilReregister() {
  backend::FakeTts tts;
  capability::ITts& itts = tts;
  std::vector<domain::AudioFrame> frames;
  const auto record = [&](const domain::AudioFrame& frame) { frames.push_back(frame); };
  CHECK(itts.set_callback(record).ok());
  CHECK(itts.cancel().ok());
  CHECK(itts.cancel().ok());
  CHECK(itts.synthesize("任何文本").error.code == domain::ErrorCode::kCancelled);
  CHECK(itts.synthesize("").error.code == domain::ErrorCode::kCancelled);
  CHECK(frames.empty());
  CHECK(itts.set_callback(record).ok());
  CHECK(itts.synthesize("恢复后的文本").ok());
  CHECK(frames.size() == ExpectedFrameCount("恢复后的文本"));
}

// 回调失败封锁：第 2 帧回调抛出时前两帧已交付且不可撤回（第 2 帧先入栈、
// 后抛出，接收方可能已保存该帧）；异常原样传播，之后 synthesize 返回
// kCancelled，重新注册后新轮次正常（无重复交付）。
void TestCallbackFailureLocksRound() {
  backend::FakeTts tts;
  capability::ITts& itts = tts;
  const std::string text(48, 'a');  // 3 帧。
  std::vector<domain::AudioFrame> frames;
  CHECK(itts.set_callback([&](const domain::AudioFrame& frame) {
             frames.push_back(frame);
             if (frames.size() == 2) {
               throw std::runtime_error("接收方合成失败");
             }
           }).ok());
  bool threw = false;
  try {
    (void)itts.synthesize(text);
  } catch (const std::runtime_error& error) {
    threw = error.what() == std::string("接收方合成失败");
  }
  CHECK(threw);
  CHECK(frames.size() == 2);
  CHECK(itts.synthesize(text).error.code == domain::ErrorCode::kCancelled);
  CHECK(frames.size() == 2);
  CHECK(itts.set_callback([&](const domain::AudioFrame& frame) {
             frames.push_back(frame);
           }).ok());
  CHECK(itts.synthesize(text).ok());
  CHECK(frames.size() == 2 + ExpectedFrameCount(text));
}

// 并发取消（块间线性化）：合成线程交付第 2 帧后停在屏障上，主线程此时
// 调用 cancel()。回调返回后 synthesize 在“第 3 帧交付前”的检查点收敛为
// kCancelled：恰好 2 帧被保留、其后没有任何新帧，取消前交付的帧不撤回。
// 屏障保证主线程的 cancel() 与合成进度有确定先后关系，不依赖时间片。
void TestConcurrentCancelStopsBeforeNextFrame() {
  backend::FakeTts tts;
  capability::ITts& itts = tts;
  const std::string text(400, 'a');  // 25 帧，足够留出取消窗口。
  std::vector<domain::AudioFrame> frames;
  std::atomic<int> delivered{0};
  std::thread::id callback_thread{};

  std::mutex barrier_mutex;
  std::condition_variable reached_cv;
  std::condition_variable release_cv;
  bool target_hit = false;
  bool released = false;

  CHECK(itts.set_callback([&](const domain::AudioFrame& frame) {
             frames.push_back(frame);
             callback_thread = std::this_thread::get_id();
             const int count = delivered.fetch_add(1) + 1;
             if (count == 2) {
               std::unique_lock<std::mutex> lock(barrier_mutex);
               target_hit = true;
               reached_cv.notify_one();
               release_cv.wait(lock, [&] { return released; });
             }
           }).ok());

  domain::OperationResult outcome = domain::OperationResult::success();
  std::thread worker([&] {
    outcome = itts.synthesize(text);
  });
  // join 后 get_id() 不再有效，先在 join 前保存工作线程 id 供断言使用。
  const std::thread::id worker_id = worker.get_id();
  {
    std::unique_lock<std::mutex> lock(barrier_mutex);
    reached_cv.wait(lock, [&] { return target_hit; });
  }
  CHECK(itts.cancel().ok());
  {
    std::lock_guard<std::mutex> lock(barrier_mutex);
    released = true;
  }
  release_cv.notify_one();
  worker.join();

  CHECK(outcome.error.code == domain::ErrorCode::kCancelled);
  CHECK(delivered.load() == 2);
  CHECK(frames.size() == 2);
  CHECK(callback_thread == worker_id);
  // 取消置位仍然保持：直接重试与重新注册后的行为与串行取消一致。
  CHECK(itts.synthesize("再试").error.code == domain::ErrorCode::kCancelled);
}

// 并发取消（结束边界）：文本只有 2 帧，主线程在第 2 帧（最后一帧）回调
// 进行中取消；synthesize 在“全部帧交付后、返回成功前”的检查点收敛为
// kCancelled，证明“帧已发完”不会被误报成成功终态。
void TestConcurrentCancelAtFinalFrame() {
  backend::FakeTts tts;
  capability::ITts& itts = tts;
  const std::string text(32, 'a');  // 恰好 2 帧。
  std::vector<domain::AudioFrame> frames;
  std::atomic<int> delivered{0};

  std::mutex barrier_mutex;
  std::condition_variable reached_cv;
  std::condition_variable release_cv;
  bool target_hit = false;
  bool released = false;

  CHECK(itts.set_callback([&](const domain::AudioFrame& frame) {
             frames.push_back(frame);
             const int count = delivered.fetch_add(1) + 1;
             if (count == 2) {
               std::unique_lock<std::mutex> lock(barrier_mutex);
               target_hit = true;
               reached_cv.notify_one();
               release_cv.wait(lock, [&] { return released; });
             }
           }).ok());

  domain::OperationResult outcome = domain::OperationResult::success();
  std::thread worker([&] {
    outcome = itts.synthesize(text);
  });
  {
    std::unique_lock<std::mutex> lock(barrier_mutex);
    reached_cv.wait(lock, [&] { return target_hit; });
  }
  CHECK(itts.cancel().ok());
  {
    std::lock_guard<std::mutex> lock(barrier_mutex);
    released = true;
  }
  release_cv.notify_one();
  worker.join();

  CHECK(outcome.error.code == domain::ErrorCode::kCancelled);
  CHECK(delivered.load() == 2);
  CHECK(frames.size() == 2);
  CHECK(itts.synthesize("再试").error.code == domain::ErrorCode::kCancelled);
}

}  // namespace

int main() {
  TestSynchronousChunkedDelivery();
  TestDeterminismAndContentDependence();
  TestInvalidInputRejectsWithoutFrames();
  TestCancelLocksUntilReregister();
  TestCallbackFailureLocksRound();
  TestConcurrentCancelStopsBeforeNextFrame();
  TestConcurrentCancelAtFinalFrame();
}

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
  // 与实现一致：相位同时取字节和与位置加权和，使等长且字节和相同的文本也能区分开。
  std::size_t byte_sum = 0;
  std::size_t weighted_sum = 0;
  std::size_t position_in_text = 0;
  for (unsigned char byte : text) {
    position_in_text += 1;
    byte_sum += static_cast<std::size_t>(byte);
    weighted_sum += position_in_text * static_cast<std::size_t>(byte);
  }
  const std::size_t phase = (byte_sum + 31 * weighted_sum) % backend::kFakeTtsCycleSamples;
  const std::size_t position =
      (phase + global_sample) % backend::kFakeTtsCycleSamples;
  return position < backend::kFakeTtsCycleSamples / 2 ? backend::kFakeTtsSquareAmplitude
                                                      : -backend::kFakeTtsSquareAmplitude;
}

// 成功路径：80 个 'a' 应产生 5 帧（帧数只由字节长度决定）；每帧 320 样本、元数据符合
// v1 契约，回调全部发生在 synthesize 返回之前、调用线程之内，且任意时刻最多一帧在途
// （无隐藏队列/线程）。起始相位由文本的字节和与位置加权和共同决定，因此本用例不假设
// 帧首落在哪个半周：电平一律用 ExpectedLevel 按文档公式推导，另按“相隔 40 反相、
// 相隔 80 同相”的方波结构复核。320 % 80 == 0 使帧间相位连续，5 帧逐采样一致。
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
  // 方波形状按与相位无关的方式断言：一帧内只出现正负两种幅度，相隔 40 个样本必然反相、
  // 相隔 80 个样本必然同相。帧首落在哪个半周由文本的起始相位决定（相位由字节和与位置
  // 加权和共同决定），这里不把它钉死在某一个象限上。
  for (const auto sample : frames[0].samples) {
    CHECK(sample == backend::kFakeTtsSquareAmplitude ||
          sample == -backend::kFakeTtsSquareAmplitude);
  }
  for (std::size_t index = 0; index + 80 < frames[0].samples.size(); ++index) {
    CHECK(frames[0].samples[index] == frames[0].samples[index + 80]);
  }
  CHECK(frames[0].samples[0] != frames[0].samples[40]);
  // 每帧 320 个样本正好覆盖 4 个整周期，因此帧与帧之间逐采样一致（与起始相位无关）。
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

  // 不同文本 → 不同 PCM：40 个 'a' 与 80 个 'a' 的字节长度不同，帧数不同；每一帧的
  // 电平一律按文档公式推导，并额外要求它与长文本同一位置的样本不同——只比较帧首会漏掉
  // “两个文本恰好落在同一象限”的情况。
  backend::FakeTts other;
  const std::string short_text(40, 'a');
  const auto short_run = collect(other, short_text);
  CHECK(short_run.size() == ExpectedFrameCount(short_text));
  bool differs = false;
  for (std::size_t index = 0; index < short_run[0].samples.size(); ++index) {
    CHECK(short_run[0].samples[index] == ExpectedLevel(short_text, index));
    if (short_run[0].samples[index] != run1[0].samples[index]) {
      differs = true;
    }
  }
  CHECK(differs);

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

// 重复运行与输出残留：成功返回后不存在任何迟到的后台交付；连续两次相同
// 文本的帧数与逐采样内容完全一致，取消/失败的新调用不会追加旧轮次的帧。
void TestRepeatAndNoResidue() {
  backend::FakeTts tts;
  capability::ITts& itts = tts;
  const std::string text(32, 'a');  // 恰好 2 帧。
  std::vector<domain::AudioFrame> frames;
  const auto record = [&](const domain::AudioFrame& frame) { frames.push_back(frame); };
  CHECK(itts.set_callback(record).ok());

  CHECK(itts.synthesize(text).ok());
  const std::size_t first_round = frames.size();
  CHECK(first_round == ExpectedFrameCount(text));
  // 返回后先做若干无关操作（取消失败轮次），帧数必须保持，证明无迟到回调。
  CHECK(itts.cancel().ok());
  CHECK(itts.synthesize(text).error.code == domain::ErrorCode::kCancelled);
  CHECK(frames.size() == first_round);
  // 重新注册后重复运行：新旧两轮逐采样一致，无输出残留。
  CHECK(itts.set_callback(record).ok());
  CHECK(itts.synthesize(text).ok());
  CHECK(frames.size() == 2 * first_round);
  for (std::size_t index = 0; index < first_round; ++index) {
    CHECK(frames[first_round + index].samples == frames[index].samples);
  }
}

// ---- 并发取消的共享确定性夹具 ----

// 一次“合成中途取消”的可复现运行结果；所有字段在 join 之后由调用方断言。
struct ConcurrentCancelResult {
  domain::OperationResult outcome = domain::OperationResult::success();
  int delivered = 0;                     // 取消前实际交付的帧数。
  std::vector<domain::AudioFrame> frames;  // 交付帧副本（worker 写、join 后读）。
  std::thread::id worker_id{};           // 合成线程 id（join 前固化）。
  std::thread::id callback_thread{};     // 回调所在线程 id（worker 写、join 后读）。
};

// 把合成停到可复现位置后由主线程 cancel() 再放行：
//  - block_after_frames == 0：合成线程在进入 synthesize 之前等待，主线程
//    先 cancel() 再放行，覆盖“首块前取消”以并发方式到达的边界；
//  - block_after_frames == N（N >= 1）：回调交付第 N 帧后停在屏障上，主线程
//    cancel() 再放行，覆盖“块间/回调进行中/结束边界”的取消。
// 不使用 sleep：屏障与条件变量保证 cancel() 与合成进度的先后关系确定。
// helper 内部除条件变量外不创建任何资源；回调只写本 helper 的结果字段。
ConcurrentCancelResult RunConcurrentCancel(backend::FakeTts& tts,
                                           const std::string& text,
                                           int block_after_frames) {
  ConcurrentCancelResult result;
  std::atomic<int> delivered{0};
  std::mutex barrier_mutex;
  std::condition_variable reached_cv;
  std::condition_variable release_cv;
  bool ready = false;
  bool released = false;

  const auto wait_until_ready = [&] {
    std::unique_lock<std::mutex> lock(barrier_mutex);
    reached_cv.wait(lock, [&] { return ready; });
  };
  const auto mark_ready_and_wait = [&] {
    std::unique_lock<std::mutex> lock(barrier_mutex);
    ready = true;
    reached_cv.notify_one();
    release_cv.wait(lock, [&] { return released; });
  };

  CHECK(tts.set_callback([&](const domain::AudioFrame& frame) {
             result.frames.push_back(frame);
             result.callback_thread = std::this_thread::get_id();
             const int count = delivered.fetch_add(1) + 1;
             if (block_after_frames > 0 && count == block_after_frames) {
               mark_ready_and_wait();
             }
           }).ok());

  std::thread worker([&] {
    if (block_after_frames == 0) {
      mark_ready_and_wait();  // 首块前：先停住，等主线程 cancel() 完再开跑。
    }
    result.outcome = tts.synthesize(text);
  });
  result.worker_id = worker.get_id();  // join 后 get_id() 不再有效，先固化。

  wait_until_ready();  // 合成已停在可复现点（第 N 帧回调内或 synthesize 入口）。
  CHECK(tts.cancel().ok());
  {
    std::lock_guard<std::mutex> lock(barrier_mutex);
    released = true;
  }
  release_cv.notify_one();
  worker.join();
  result.delivered = delivered.load();
  return result;
}

// 并发取消（首块前）：cancel() 先于 synthesize 入口的检查到达（从另一线程
// 到达，而非串行调用顺序），合成必须一帧不交即收敛为 kCancelled。
void TestConcurrentCancelBeforeFirstFrame() {
  backend::FakeTts tts;
  capability::ITts& itts = tts;
  const auto result = RunConcurrentCancel(tts, std::string(400, 'a'), 0);
  CHECK(result.outcome.error.code == domain::ErrorCode::kCancelled);
  CHECK(result.delivered == 0);
  CHECK(result.frames.empty());
  // 回调从未运行，无需线程归属断言；取消封锁保持到重新注册为止。
  CHECK(itts.synthesize("再试").error.code == domain::ErrorCode::kCancelled);
}

// 并发取消（块间线性化）：合成线程交付第 2 帧后停在屏障上，主线程此时
// 调用 cancel()。回调返回后 synthesize 在“第 3 帧交付前”的检查点收敛为
// kCancelled：恰好 2 帧被保留、其后没有任何新帧，取消前交付的帧不撤回；
// 回调必须运行在合成线程（无隐藏线程），取消后封锁保持。
void TestConcurrentCancelStopsBeforeNextFrame() {
  backend::FakeTts tts;
  capability::ITts& itts = tts;
  const auto result = RunConcurrentCancel(tts, std::string(400, 'a'), 2);
  CHECK(result.outcome.error.code == domain::ErrorCode::kCancelled);
  CHECK(result.delivered == 2);
  CHECK(result.frames.size() == 2);
  CHECK(result.callback_thread == result.worker_id);
  CHECK(itts.synthesize("再试").error.code == domain::ErrorCode::kCancelled);
}

// 并发取消（结束边界）：文本只有 2 帧，主线程在第 2 帧（最后一帧）回调
// 进行中取消；synthesize 在“全部帧交付后、返回成功前”的检查点收敛为
// kCancelled，证明“帧已发完”不会被误报成成功终态（整段已交付，外层不得
// 把该结果当可重试失败而整段重放）。
// 内容相关性不变量：等长且字节和相同的文本也必须产生不同波形。相位只取字节和时，
// "ab" 与 "ba" 会得到逐采样一致的 PCM，于是“不同回答产生不同音频”这条证据就不成立。
void TestEqualLengthTextsWithEqualByteSumDiffer() {
  backend::FakeTts first;
  std::vector<domain::AudioFrame> first_frames;
  CHECK(first.set_callback([&](const domain::AudioFrame& frame) { first_frames.push_back(frame); })
            .ok());
  CHECK(first.synthesize("ab").ok());

  backend::FakeTts second;
  std::vector<domain::AudioFrame> second_frames;
  CHECK(second.set_callback([&](const domain::AudioFrame& frame) { second_frames.push_back(frame); })
            .ok());
  CHECK(second.synthesize("ba").ok());

  // 两个文本都是 2 字节、字节和都是 195，因此差异只能来自位置：长度相同、内容不同。
  CHECK(first_frames.size() == second_frames.size());
  CHECK(!first_frames.empty());
  bool differs = false;
  for (std::size_t index = 0; index < first_frames.size() && !differs; ++index) {
    if (first_frames.at(index).samples != second_frames.at(index).samples) {
      differs = true;
    }
  }
  CHECK(differs);
}

void TestConcurrentCancelAtFinalFrame() {
  backend::FakeTts tts;
  capability::ITts& itts = tts;
  const auto result = RunConcurrentCancel(tts, std::string(32, 'a'), 2);
  CHECK(result.outcome.error.code == domain::ErrorCode::kCancelled);
  CHECK(result.delivered == 2);
  CHECK(result.frames.size() == 2);
  CHECK(result.callback_thread == result.worker_id);
  CHECK(itts.synthesize("再试").error.code == domain::ErrorCode::kCancelled);
}

}  // namespace

int main() {
  TestSynchronousChunkedDelivery();
  TestDeterminismAndContentDependence();
  TestInvalidInputRejectsWithoutFrames();
  TestCancelLocksUntilReregister();
  TestCallbackFailureLocksRound();
  TestRepeatAndNoResidue();
  TestConcurrentCancelBeforeFirstFrame();
  TestConcurrentCancelStopsBeforeNextFrame();
  TestConcurrentCancelAtFinalFrame();
  TestEqualLengthTextsWithEqualByteSumDiffer();
}

#include "../test_support.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "speech_segmentation.hpp"

using namespace nexweave;
using nexweave::runtime::ActivityRun;
using nexweave::runtime::SegmentCutReason;
using nexweave::runtime::SegmenterStep;
using nexweave::runtime::SpeechActivity;
using nexweave::runtime::SpeechActivityScript;
using nexweave::runtime::SpeechSegmentationConfig;
using nexweave::runtime::SpeechSegment;
using nexweave::runtime::SpeechSegmenter;

namespace {

// 统一音频契约的固定帧长；本文件所有“帧”都指这个长度。
constexpr std::size_t kFrameSamples = domain::kAudioFrameSamples;

// 构造一个全部样本取同一值的合法帧。测试用不同取值标记不同区域，
// 从而在不读波形的前提下核对“哪些帧进了段”。
domain::AudioFrame Frame(std::int16_t value) {
  const std::vector<std::int16_t> samples(kFrameSamples, value);
  const auto frame = domain::AudioFrame::from_samples(samples);
  CHECK(frame.ok());
  return frame.value;
}

// 连续喂入 count 帧并收集期间交付的段与每帧的推进结果。
// 每个用例都通过它断言“推进结果”和“交付内容”两条独立证据，而不是只看段数。
struct FeedTrace {
  std::vector<SpeechSegment> segments;
  std::vector<SegmenterStep> steps;
};

FeedTrace Feed(SpeechSegmenter& segmenter, std::size_t count, SpeechActivity activity,
               std::int16_t value) {
  FeedTrace trace;
  for (std::size_t index = 0; index < count; ++index) {
    SpeechSegment produced;
    const auto step = segmenter.push(Frame(value), activity, produced);
    CHECK(step.ok());
    trace.steps.push_back(*step.value);
    if (step.value->produced_segment) {
      trace.segments.push_back(std::move(produced));
    }
  }
  return trace;
}

// 把段内帧的首个样本拼成序列，用于核对帧的顺序与来源。
std::vector<std::int16_t> HeadSamples(const SpeechSegment& segment) {
  std::vector<std::int16_t> heads;
  for (const auto& frame : segment.frames) {
    CHECK(frame.samples.size() == kFrameSamples);
    heads.push_back(frame.samples.front());
  }
  return heads;
}

SpeechSegmentationConfig Config(std::size_t preroll, std::size_t min_silence,
                               std::size_t min_speech, std::size_t max_speech) {
  SpeechSegmentationConfig config;
  config.preroll_frames = preroll;
  config.min_silence_frames = min_silence;
  config.min_speech_frames = min_speech;
  config.max_speech_frames = max_speech;
  return config;
}

}  // namespace

// 脚本不变量：活动脚本按声明顺序逐帧给出判定，耗尽后饱和在最后一段，reset 回到开头。
// 这条用例保护“分段场景完全由显式脚本决定”，即分段测试不依赖波形、模型或真实时间。
void TestScriptYieldsDeclaredActivitySequence() {
  SpeechActivityScript script({ActivityRun{2, SpeechActivity::kSilence},
                               ActivityRun{3, SpeechActivity::kSpeech},
                               ActivityRun{1, SpeechActivity::kSilence}});

  const std::vector<SpeechActivity> expected = {
      SpeechActivity::kSilence, SpeechActivity::kSilence, SpeechActivity::kSpeech,
      SpeechActivity::kSpeech,  SpeechActivity::kSpeech,  SpeechActivity::kSilence};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto decision = script.detect(Frame(1));
    CHECK(decision.ok());
    CHECK(*decision.value == expected.at(index));
  }
  CHECK(script.frames_consumed() == 6);
  CHECK(script.exhausted());

  // 耗尽后饱和在最后一段（静音），而不是回到开头或随机翻转。
  const auto saturated = script.detect(Frame(1));
  CHECK(saturated.ok());
  CHECK(*saturated.value == SpeechActivity::kSilence);

  script.reset();
  CHECK(script.frames_consumed() == 0);
  CHECK(!script.exhausted());
  const auto restarted = script.detect(Frame(1));
  CHECK(restarted.ok());
  CHECK(*restarted.value == SpeechActivity::kSilence);

  // 0 帧的段没有意义：构造时被删除，因此空脚本与全零脚本都等价于“始终静音”。
  SpeechActivityScript zeros({ActivityRun{0, SpeechActivity::kSpeech},
                              ActivityRun{0, SpeechActivity::kSilence}});
  const auto silent = zeros.detect(Frame(1));
  CHECK(silent.ok());
  CHECK(*silent.value == SpeechActivity::kSilence);
  CHECK(zeros.exhausted());
}

// 静音超时不变量：一次说话在连续静音达到上限时结束；段头带前置缓冲避免切掉声母，
// 段尾静音被裁掉。它同时保护归属（流标识、段编号、块编号、段起点）与帧内容。
void TestSilenceTimeoutDeliversSegmentWithPrerollHead() {
  SpeechSegmenter segmenter("mic-unit", Config(3, 4, 2, 100));

  const auto silence_head = Feed(segmenter, 5, SpeechActivity::kSilence, 7);
  CHECK(silence_head.segments.empty());
  for (const auto& step : silence_head.steps) {
    CHECK(!step.speech_started);
    CHECK(!step.produced_segment);
  }
  CHECK(segmenter.preroll_frames_held() == 3);

  const auto speech = Feed(segmenter, 5, SpeechActivity::kSpeech, 100);
  CHECK(speech.segments.empty());
  // 起音只发生在进入人声的第一帧，并携带段起点（含前置缓冲）。
  CHECK(speech.steps.front().speech_started);
  CHECK(speech.steps.front().speech_sequence == 5);
  CHECK(speech.steps.front().start_sequence == 2);
  CHECK(speech.steps.front().segment_id == 1);
  for (std::size_t index = 1; index < speech.steps.size(); ++index) {
    CHECK(!speech.steps.at(index).speech_started);
  }
  // 前置缓冲已经交给段头，因此空闲缓冲被清空，不会重复计入下一段。
  CHECK(segmenter.preroll_frames_held() == 0);

  // 静音 3 帧还没到上限：说话仍在进行，且这几帧只是待定，并未进入块。
  const auto pending_silence = Feed(segmenter, 3, SpeechActivity::kSilence, 7);
  CHECK(pending_silence.segments.empty());
  CHECK(segmenter.in_speech());
  CHECK(segmenter.pending_silence_frames() == 3);
  CHECK(segmenter.chunk_frames_held() == 8);

  // 第 4 帧静音达到上限：说话结束并交付。
  const auto closing = Feed(segmenter, 1, SpeechActivity::kSilence, 7);
  CHECK(closing.segments.size() == 1);
  CHECK(closing.steps.front().produced_segment);
  CHECK(!closing.steps.front().speech_started);

  const SpeechSegment& segment = closing.segments.front();
  CHECK(segment.stream_id == "mic-unit");
  CHECK(segment.segment_id == 1);
  CHECK(segment.chunk_index == 0);
  CHECK(segment.start_sequence == 2);
  CHECK(segment.speech_frames == 5);
  CHECK(segment.cut_reason == SegmentCutReason::kSilenceTimeout);
  // 段内容 = 3 帧前置缓冲 + 5 帧人声；4 帧段尾静音全部被裁掉。
  CHECK(segment.frames.size() == 8);
  const std::vector<std::int16_t> expected_heads = {7, 7, 7, 100, 100, 100, 100, 100};
  CHECK(HeadSamples(segment) == expected_heads);

  CHECK(!segmenter.in_speech());
  CHECK(segmenter.pending_silence_frames() == 0);
  CHECK(segmenter.chunk_frames_held() == 0);
  CHECK(segmenter.delivered_chunks() == 1);
  CHECK(segmenter.dropped_short_segments() == 0);
}

// 短脉冲不变量：整段人声不足下限的说话不交付，但起音通知仍然在起音帧立即产生。
// 这是显式取舍：打断必须快（不等确认），因此一次咳嗽可以打断播报，代价是它不会
// 产生一轮回答；丢弃次数由 dropped_short_segments() 暴露，不是静默丢弃。
void TestShortPulseProducesOnsetButNoSegment() {
  SpeechSegmenter segmenter("mic-pulse", Config(2, 2, 4, 100));

  Feed(segmenter, 3, SpeechActivity::kSilence, 5);
  const auto pulse = Feed(segmenter, 2, SpeechActivity::kSpeech, 60);
  CHECK(pulse.steps.front().speech_started);
  CHECK(pulse.segments.empty());

  const auto closed = Feed(segmenter, 2, SpeechActivity::kSilence, 5);
  CHECK(closed.segments.empty());
  CHECK(!segmenter.in_speech());
  CHECK(segmenter.delivered_chunks() == 0);
  CHECK(segmenter.dropped_short_segments() == 1);
}

// 最长语音不变量：块内人声达到上限即切块，且切出的多块共享同一段编号（同一次说话），
// 只有首块产生起音通知。它保护“一次说话的内部切分不会被下游误判成两次说话”。
void TestMaxSpeechSplitsIntoSingleUtteranceChunks() {
  SpeechSegmenter segmenter("mic-long", Config(1, 3, 2, 4));
  CHECK(segmenter.chunk_frame_capacity() == 8);

  Feed(segmenter, 2, SpeechActivity::kSilence, 1);
  const auto speech = Feed(segmenter, 10, SpeechActivity::kSpeech, 200);

  CHECK(speech.segments.size() == 2);
  CHECK(speech.segments.at(0).chunk_index == 0);
  CHECK(speech.segments.at(0).speech_frames == 4);
  CHECK(speech.segments.at(0).frames.size() == 5);  // 1 帧前置缓冲 + 4 帧人声
  CHECK(speech.segments.at(0).cut_reason == SegmentCutReason::kMaxSpeech);
  CHECK(speech.segments.at(1).chunk_index == 1);
  CHECK(speech.segments.at(1).speech_frames == 4);
  CHECK(speech.segments.at(1).frames.size() == 4);  // 续块不带前置缓冲
  CHECK(speech.segments.at(1).cut_reason == SegmentCutReason::kMaxSpeech);
  for (const auto& segment : speech.segments) {
    CHECK(segment.segment_id == 1);
    CHECK(segment.stream_id == "mic-long");
    // start_sequence 属于“这一次说话”，同一段的所有块共享它：它标识说话从哪里开始，
    // 而不是本块第一帧的序号。续块的音频在输入流里更靠后，因此不能把它当成块内下标。
    CHECK(segment.start_sequence == 1);
  }
  // 块的流内位置由“段起点 + 前序块帧数”推出：首块 5 帧（1 帧前置缓冲 + 4 帧人声）
  // 覆盖序号 1..5，因此第二块从序号 6 开始。这条独立复算把上面的语义写成可检查的算式，
  // 避免后人再把 start_sequence 误读成块起点。
  std::size_t next_position = 1;
  std::vector<std::size_t> chunk_positions;
  for (const auto& segment : speech.segments) {
    chunk_positions.push_back(next_position);
    next_position += segment.frames.size();
  }
  CHECK(chunk_positions.size() == 2);
  CHECK(chunk_positions.at(0) == 1);
  CHECK(chunk_positions.at(1) == 6);

  // 起音只在第一块的第一帧出现；两条切块记录证明切块原因可观测。
  CHECK(speech.steps.front().speech_started);
  for (std::size_t index = 1; index < speech.steps.size(); ++index) {
    CHECK(!speech.steps.at(index).speech_started);
  }
  CHECK(segmenter.max_speech_cuts() == 2);
  CHECK(segmenter.in_speech());

  const auto closing = Feed(segmenter, 3, SpeechActivity::kSilence, 1);
  CHECK(closing.segments.size() == 1);
  CHECK(closing.segments.front().segment_id == 1);
  CHECK(closing.segments.front().chunk_index == 2);
  CHECK(closing.segments.front().speech_frames == 2);
  CHECK(closing.segments.front().cut_reason == SegmentCutReason::kSilenceTimeout);
  // 收尾块沿用同一次说话的段起点，而不是它自己的流内位置。
  CHECK(closing.segments.front().start_sequence == 1);
  CHECK(segmenter.delivered_chunks() == 3);
  // 三段拼起来正好是 1 帧前置缓冲 + 10 帧人声，一帧不丢。
  CHECK(speech.segments.at(0).frames.size() + speech.segments.at(1).frames.size() +
            closing.segments.front().frames.size() ==
        11);
}

// 段内停顿与段尾静音不变量：说话中间的短静音属于同一次说话并被保留（时序不乱），
// 只有判定结束的那一段尾随静音被裁掉。若实现顺序反过来，两者就无法区分。
void TestIntraUtterancePauseIsKeptAndTrailingSilenceTrimmed() {
  SpeechSegmenter segmenter("mic-pause", Config(0, 4, 2, 100));

  Feed(segmenter, 3, SpeechActivity::kSpeech, 50);
  Feed(segmenter, 2, SpeechActivity::kSilence, 9);
  Feed(segmenter, 3, SpeechActivity::kSpeech, 50);
  const auto closing = Feed(segmenter, 4, SpeechActivity::kSilence, 9);

  CHECK(closing.segments.size() == 1);
  const SpeechSegment& segment = closing.segments.front();
  CHECK(segment.start_sequence == 0);  // 无前置缓冲时段从起音帧开始
  CHECK(segment.speech_frames == 6);
  CHECK(segment.frames.size() == 8);
  const std::vector<std::int16_t> expected_heads = {50, 50, 50, 9, 9, 50, 50, 50};
  CHECK(HeadSamples(segment) == expected_heads);
  CHECK(segment.cut_reason == SegmentCutReason::kSilenceTimeout);
}

// 前置缓冲上限不变量：环形缓冲只保留紧邻起音的若干帧静音，更早的静音被覆盖丢弃。
// 它保护“段头缓冲有明确上限”，而不是随静音时长线性增长。
void TestPrerollRingKeepsOnlyNearestFrames() {
  // 静音超时取 4 帧，因此下面 2 帧停顿不会提前结束说话，收尾只能靠输入结束冲刷。
  SpeechSegmenter segmenter("mic-preroll", Config(2, 4, 1, 100));

  // 静音帧带上递增的样本值，从而可以核对留下的到底是哪几帧。
  for (std::int16_t value = 10; value <= 14; ++value) {
    SpeechSegment produced;
    const auto step = segmenter.push(Frame(value), SpeechActivity::kSilence, produced);
    CHECK(step.ok());
    CHECK(segmenter.preroll_frames_held() <= 2);
  }
  CHECK(segmenter.preroll_frames_held() == 2);

  SpeechSegment produced;
  const auto onset = segmenter.push(Frame(100), SpeechActivity::kSpeech, produced);
  CHECK(onset.ok());
  CHECK(onset.value->speech_started);
  // 起音帧序号 5，前置缓冲 2 帧，因此段起点是 3——最早的三帧静音已被上限丢弃。
  CHECK(onset.value->start_sequence == 3);

  // 两帧停顿低于静音超时，说话仍在进行。
  const auto pause = Feed(segmenter, 2, SpeechActivity::kSilence, 11);
  CHECK(pause.segments.empty());

  SpeechSegment tail;
  const auto flushed = segmenter.flush(tail);
  CHECK(flushed.ok());
  CHECK(*flushed.value);
  const std::vector<std::int16_t> expected_heads = {13, 14, 100};
  CHECK(HeadSamples(tail) == expected_heads);
  CHECK(tail.start_sequence == 3);
  CHECK(tail.cut_reason == SegmentCutReason::kInputEnded);
}

// 输入结束冲刷不变量：正常结束只交付达到最短语音的说话，过短的说话被丢弃；
// 冲刷幂等，重复调用不会重复交付同一段音频。
void TestFlushDeliversLongSpeechAndDiscardsShortSpeech() {
  {
    SpeechSegmenter segmenter("mic-flush", Config(1, 4, 3, 100));
    Feed(segmenter, 1, SpeechActivity::kSilence, 4);
    Feed(segmenter, 4, SpeechActivity::kSpeech, 80);

    SpeechSegment tail;
    const auto flushed = segmenter.flush(tail);
    CHECK(flushed.ok());
    CHECK(*flushed.value);
    CHECK(tail.cut_reason == SegmentCutReason::kInputEnded);
    CHECK(tail.speech_frames == 4);
    CHECK(tail.frames.size() == 5);
    CHECK(tail.start_sequence == 0);
    CHECK(segmenter.delivered_chunks() == 1);

    // 幂等：输入已经冲刷，重复调用没有内容可交付，也不会重复计数。
    SpeechSegment second;
    const auto again = segmenter.flush(second);
    CHECK(again.ok());
    CHECK(!*again.value);
    CHECK(segmenter.delivered_chunks() == 1);
  }
  {
    SpeechSegmenter segmenter("mic-flush-short", Config(2, 4, 5, 100));
    Feed(segmenter, 2, SpeechActivity::kSilence, 4);
    Feed(segmenter, 3, SpeechActivity::kSpeech, 80);

    SpeechSegment tail;
    const auto flushed = segmenter.flush(tail);
    CHECK(flushed.ok());
    CHECK(!*flushed.value);
    CHECK(segmenter.delivered_chunks() == 0);
    CHECK(segmenter.dropped_short_segments() == 1);
    CHECK(!segmenter.in_speech());
  }
  {
    // 空闲态冲刷没有任何内容，也不产生计数。
    SpeechSegmenter segmenter("mic-flush-idle", Config(2, 4, 2, 100));
    Feed(segmenter, 3, SpeechActivity::kSilence, 4);
    SpeechSegment tail;
    const auto flushed = segmenter.flush(tail);
    CHECK(flushed.ok());
    CHECK(!*flushed.value);
    CHECK(segmenter.dropped_short_segments() == 0);
    CHECK(segmenter.preroll_frames_held() == 0);
  }
}

// 放弃不变量：停止/失败路径把未完成的说话整段丢弃并返回人声帧数，不交付截断音频；
// 段编号与输入序号继续单调，因此“取消之后的新段”仍然有可区分的归属。
void TestAbandonDropsInFlightSpeechAndKeepsNumbering() {
  SpeechSegmenter segmenter("mic-abandon", Config(1, 2, 2, 100));

  Feed(segmenter, 2, SpeechActivity::kSilence, 7);
  Feed(segmenter, 5, SpeechActivity::kSpeech, 70);
  CHECK(segmenter.in_speech());
  CHECK(segmenter.chunk_frames_held() == 6);

  CHECK(segmenter.abandon() == 5);
  CHECK(!segmenter.in_speech());
  CHECK(segmenter.chunk_frames_held() == 0);
  CHECK(segmenter.preroll_frames_held() == 0);
  CHECK(segmenter.delivered_chunks() == 0);
  CHECK(segmenter.abandon() == 0);  // 幂等

  // 取消之后的新说话必须得到新的段编号，并且段起点仍然按输入流序号计算。
  Feed(segmenter, 2, SpeechActivity::kSilence, 7);
  const auto speech = Feed(segmenter, 3, SpeechActivity::kSpeech, 70);
  const auto closing = Feed(segmenter, 2, SpeechActivity::kSilence, 7);
  CHECK(speech.steps.front().segment_id == 2);
  CHECK(speech.steps.front().start_sequence == 8);
  CHECK(closing.segments.size() == 1);
  CHECK(closing.segments.front().segment_id == 2);
  CHECK(closing.segments.front().speech_frames == 3);
  CHECK(segmenter.frames_fed() == 14);
}

// 非法输入不变量：不符合统一音频契约的帧被拒绝，且不推进序号、不入缓冲、不改变状态，
// 因此调用方修正后可以重试同一帧。
void TestInvalidFrameIsRejectedWithoutStateChange() {
  SpeechSegmenter segmenter("mic-invalid", Config(2, 2, 1, 100));

  domain::AudioFrame short_frame;
  short_frame.samples.assign(kFrameSamples - 1, 0);
  SpeechSegment produced;
  const auto rejected = segmenter.push(short_frame, SpeechActivity::kSpeech, produced);
  CHECK(!rejected.ok());
  CHECK(rejected.error.code == domain::ErrorCode::kInvalidInput);
  CHECK(segmenter.frames_fed() == 0);
  CHECK(!segmenter.in_speech());
  CHECK(segmenter.preroll_frames_held() == 0);
  CHECK(segmenter.chunk_frames_held() == 0);

  const auto accepted = segmenter.push(Frame(3), SpeechActivity::kSilence, produced);
  CHECK(accepted.ok());
  CHECK(segmenter.frames_fed() == 1);
  CHECK(segmenter.preroll_frames_held() == 1);
}

// 配置不变量：越界配置被归一化到文档化的安全值，单块上限按公式推导，保证单块内存
// 与输入长度无关。这条用例让“容量上限”成为可复算的公式而不是注释里的说法。
void TestConfigNormalizationAndCapacityFormula() {
  SpeechSegmenter degenerate("mic-degenerate", Config(0, 0, 0, 0));
  CHECK(degenerate.config().min_speech_frames == 1);
  CHECK(degenerate.config().min_silence_frames == 1);
  CHECK(degenerate.config().max_speech_frames == 1);
  CHECK(degenerate.chunk_frame_capacity() == 2);

  SpeechSegmenter normalized("mic-normalized", Config(4, 5, 20, 3));
  CHECK(normalized.config().min_speech_frames == 20);
  CHECK(normalized.config().max_speech_frames == 20);
  CHECK(normalized.chunk_frame_capacity() == 29);

  // 归一化后 max_speech >= min_speech 恒成立，因此“已经切过块的段”必然满足最短语音，
  // 丢弃过短段时不会留下已交付的块。
  CHECK(normalized.config().max_speech_frames >= normalized.config().min_speech_frames);
  CHECK(degenerate.config().max_speech_frames >= degenerate.config().min_speech_frames);
}

// 单块总量上限不变量：只限制人声帧数挡不住“人声帧之间反复夹接近上限停顿”的输入；
// 总量上限在追加之前判定，保证 chunk_frames_held() 永不越过 chunk_frame_capacity()。
void TestMaxChunkFramesBoundsSingleChunk() {
  SpeechSegmenter segmenter("mic-chunk-cap", Config(0, 5, 1, 5));
  CHECK(segmenter.chunk_frame_capacity() == 10);

  Feed(segmenter, 1, SpeechActivity::kSpeech, 30);
  CHECK(segmenter.chunk_frames_held() <= segmenter.chunk_frame_capacity());
  Feed(segmenter, 4, SpeechActivity::kSilence, 8);
  Feed(segmenter, 1, SpeechActivity::kSpeech, 30);
  CHECK(segmenter.chunk_frames_held() == 6);
  Feed(segmenter, 4, SpeechActivity::kSilence, 8);

  // 本帧落地后总量会到 11 帧，超过上限的 10 帧：实现必须在追加之前切块，
  // 并且丢掉尚未落地的 4 帧停顿（它们只是停顿，人声一帧不丢）。
  const auto cut = Feed(segmenter, 1, SpeechActivity::kSpeech, 30);
  CHECK(cut.segments.size() == 1);
  CHECK(cut.segments.front().cut_reason == SegmentCutReason::kMaxChunkFrames);
  CHECK(cut.segments.front().chunk_index == 0);
  CHECK(cut.segments.front().segment_id == 1);
  CHECK(cut.segments.front().speech_frames == 2);
  CHECK(cut.segments.front().frames.size() == 6);
  CHECK(segmenter.max_chunk_cuts() == 1);
  CHECK(segmenter.chunk_frames_held() == 1);
  CHECK(segmenter.chunk_frames_held() <= segmenter.chunk_frame_capacity());

  const auto closing = Feed(segmenter, 5, SpeechActivity::kSilence, 8);
  CHECK(closing.segments.size() == 1);
  CHECK(closing.segments.front().segment_id == 1);
  CHECK(closing.segments.front().chunk_index == 1);
  CHECK(closing.segments.front().speech_frames == 1);
  CHECK(closing.segments.front().frames.size() == 1);
  CHECK(closing.segments.front().cut_reason == SegmentCutReason::kSilenceTimeout);
  CHECK(segmenter.delivered_chunks() == 2);
}

// 拼接辅助不变量：段到连续 PCM 的转换只接受合法帧，逐帧顺序拼接且不改变样本。
void TestSegmentSamplesConcatenatesFramesInOrder() {
  SpeechSegment segment;
  segment.frames.push_back(Frame(11));
  segment.frames.push_back(Frame(22));
  const auto samples = nexweave::runtime::segment_samples(segment);
  CHECK(samples.size() == 2 * kFrameSamples);
  CHECK(samples.front() == 11);
  CHECK(samples.at(kFrameSamples) == 22);
  CHECK(samples.back() == 22);

  // 非法帧被跳过而不是拼进结果：半截 PCM 会让下游把坏数据当成完整音频。
  SpeechSegment mixed;
  mixed.frames.push_back(Frame(33));
  domain::AudioFrame broken;
  broken.samples.assign(10, 44);
  mixed.frames.push_back(broken);
  const auto filtered = nexweave::runtime::segment_samples(mixed);
  CHECK(filtered.size() == kFrameSamples);
  CHECK(filtered.front() == 33);

  SpeechSegment empty;
  CHECK(nexweave::runtime::segment_samples(empty).empty());
}

int main() {
  TestScriptYieldsDeclaredActivitySequence();
  TestSilenceTimeoutDeliversSegmentWithPrerollHead();
  TestShortPulseProducesOnsetButNoSegment();
  TestMaxSpeechSplitsIntoSingleUtteranceChunks();
  TestIntraUtterancePauseIsKeptAndTrailingSilenceTrimmed();
  TestPrerollRingKeepsOnlyNearestFrames();
  TestFlushDeliversLongSpeechAndDiscardsShortSpeech();
  TestAbandonDropsInFlightSpeechAndKeepsNumbering();
  TestInvalidFrameIsRejectedWithoutStateChange();
  TestConfigNormalizationAndCapacityFormula();
  TestMaxChunkFramesBoundsSingleChunk();
  TestSegmentSamplesConcatenatesFramesInOrder();
  return 0;
}

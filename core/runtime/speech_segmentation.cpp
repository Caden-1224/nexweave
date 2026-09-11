#include "speech_segmentation.hpp"

#include <algorithm>
#include <utility>

namespace nexweave::runtime {
namespace {

// 配置归一化：非法值回退到文档化的安全值，而不是产生畸形分段。
// 顺序理由：先抬高两个下限，再抬高 max_speech_frames，保证
// “max_speech_frames >= min_speech_frames” 这条不变量在使用前已经成立，
// 后续判据不需要重复校验。
SpeechSegmentationConfig normalize_config(SpeechSegmentationConfig config) {
  if (config.min_speech_frames == 0) {
    config.min_speech_frames = 1;
  }
  if (config.min_silence_frames == 0) {
    config.min_silence_frames = 1;
  }
  if (config.max_speech_frames < config.min_speech_frames) {
    config.max_speech_frames = config.min_speech_frames;
  }
  return config;
}

}  // namespace

SpeechActivityScript::SpeechActivityScript(std::vector<ActivityRun> runs)
    : runs_(std::move(runs)) {
  // 0 帧的段没有意义，构造时直接删除，避免 detect 里出现“推进 0 帧”的空转分支。
  runs_.erase(std::remove_if(runs_.begin(), runs_.end(),
                             [](const ActivityRun& run) { return run.frames == 0; }),
              runs_.end());
}

domain::Result<SpeechActivity> SpeechActivityScript::detect(const domain::AudioFrame&) {
  ++frames_consumed_;
  if (runs_.empty()) {
    // 空脚本等价于“全部静音”：判定只模拟事件，不声称识别了波形中的人声。
    return domain::Result<SpeechActivity>::success(SpeechActivity::kSilence);
  }
  while (run_index_ < runs_.size() && used_in_run_ >= runs_.at(run_index_).frames) {
    used_in_run_ = 0;
    ++run_index_;
  }
  if (run_index_ >= runs_.size()) {
    // 饱和在最后一段：脚本耗尽后不会突然翻转为静音，超长输入的行为因此可预测。
    return domain::Result<SpeechActivity>::success(runs_.back().activity);
  }
  ++used_in_run_;
  return domain::Result<SpeechActivity>::success(runs_.at(run_index_).activity);
}

void SpeechActivityScript::reset() {
  run_index_ = 0;
  used_in_run_ = 0;
  frames_consumed_ = 0;
}

bool SpeechActivityScript::exhausted() const noexcept {
  if (run_index_ >= runs_.size()) {
    return true;
  }
  // 游标只在“下一次 detect”时才跨过已用完的段，因此这里必须把当前段的剩余帧数一起
  // 判断：恰好读完声明帧数就应当视为耗尽，而不是要再调用一次 detect 才算。
  return runs_.at(run_index_).frames <= used_in_run_;
}

std::size_t SpeechActivityScript::frames_consumed() const noexcept {
  return frames_consumed_;
}

std::vector<std::int16_t> segment_samples(const SpeechSegment& segment) {
  std::vector<std::int16_t> samples;
  // 合法帧固定 320 样本，因此按帧数预留就是精确容量，不会重复扩容。
  samples.reserve(segment.frames.size() * domain::kAudioFrameSamples);
  for (const auto& frame : segment.frames) {
    // 非法帧被跳过而不是拼进结果：半截 PCM 会让下游把坏数据当成完整音频。
    if (!domain::validate_audio_frame(frame).ok()) {
      continue;
    }
    samples.insert(samples.end(), frame.samples.begin(), frame.samples.end());
  }
  return samples;
}

SpeechSegmenter::SpeechSegmenter(std::string stream_id, SpeechSegmentationConfig config)
    : stream_id_(std::move(stream_id)), config_(normalize_config(config)) {}

std::size_t SpeechSegmenter::chunk_frame_capacity() const noexcept {
  return config_.preroll_frames + config_.max_speech_frames + config_.min_silence_frames;
}

domain::Result<SegmenterStep> SpeechSegmenter::push(const domain::AudioFrame& frame,
                                                    SpeechActivity activity,
                                                    SpeechSegment& out) {
  // 先校验再改状态：非法帧必须可以重试，不能把序号或缓冲推进到一半。
  if (!domain::validate_audio_frame(frame).ok()) {
    return domain::Result<SegmenterStep>::failure(domain::ErrorCode::kInvalidInput,
                                                  "分段输入帧不符合统一音频契约");
  }

  SegmenterStep step;
  const std::uint64_t sequence = frames_fed_;
  ++frames_fed_;

  if (activity == SpeechActivity::kSilence) {
    if (!in_speech_) {
      // 空闲态：只维护段头前置缓冲。静音本身永远不产生交付。
      push_preroll(frame);
      return domain::Result<SegmenterStep>::success(std::move(step));
    }
    // 说话中：停顿先待定。只有后续人声确认说话继续时才落地；否则它就是段尾静音，
    // 交付时被裁掉。这个“先待定”的顺序是本类区分“段内停顿”和“段尾静音”的唯一依据。
    pending_silence_.push_back(frame);
    ++silence_run_;
    if (silence_run_ < config_.min_silence_frames) {
      return domain::Result<SegmenterStep>::success(std::move(step));
    }
    pending_silence_.clear();
    if (segment_speech_frames_ >= config_.min_speech_frames) {
      // 只有当前块真的收到过人声才交付：长度上限切块后立即静音时，最后一块是空的，
      // 交付一个空段会让下游把“没有音频”当成一次说话。
      if (chunk_speech_frames_ > 0) {
        close_chunk(SegmentCutReason::kSilenceTimeout, out);
        step.produced_segment = true;
      }
    } else {
      // 整段人声不足下限：短脉冲不是一次说话。归一下限保证“已经切过块的段”必然满足
      // 下限，因此走到这里时该段从未交付过任何块，丢弃不会留下半段输出。
      ++dropped_short_segments_;
    }
    reset_to_idle();
    return domain::Result<SegmenterStep>::success(std::move(step));
  }

  // 以下是人声帧路径。
  if (!in_speech_) {
    begin_segment(sequence, step);
  } else if (chunk_frames_.size() + pending_silence_.size() + 1 > chunk_frame_capacity()) {
    // 块内总帧数上限。必须在追加之前判断：追加时要先落地待定停顿再写入本帧，
    // 如果放到追加之后判断，单块就会超过硬上限。
    // 强制切块时尚未落地的段内停顿（最多 min_silence_frames-1 帧）被丢弃：它们只是
    // 停顿、不含用户语音内容，而且这是一条有计数（max_chunk_cuts）的显式策略，
    // 不是静默丢帧。人声帧一帧都不丢。
    pending_silence_.clear();
    close_chunk(SegmentCutReason::kMaxChunkFrames, out);
    ++max_chunk_cuts_;
    step.produced_segment = true;
    start_continuation_chunk();
  }
  // 说话中间的停顿属于同一次说话，必须在追加本帧之前落地；顺序反了就会先追加人声，
  // 让停顿排到人声之后，音频时序被打乱。
  flush_pending_silence_into_chunk();
  chunk_frames_.push_back(frame);
  ++chunk_speech_frames_;
  ++segment_speech_frames_;
  silence_run_ = 0;

  if (chunk_speech_frames_ >= config_.max_speech_frames) {
    // 人声帧数达到上限：切块并立即开始同一段说话的续块。续块不产生起音通知，
    // 否则同一次说话会被设备误判成两次打断。
    close_chunk(SegmentCutReason::kMaxSpeech, out);
    ++max_speech_cuts_;
    step.produced_segment = true;
    start_continuation_chunk();
  }
  return domain::Result<SegmenterStep>::success(std::move(step));
}

domain::Result<bool> SpeechSegmenter::flush(SpeechSegment& out) {
  const bool had_enough_speech =
      in_speech_ && segment_speech_frames_ >= config_.min_speech_frames;
  if (in_speech_ && !had_enough_speech) {
    // 输入结束时说话还没说完，而且人声本来就不足下限：按短脉冲丢弃。
    ++dropped_short_segments_;
  }
  // 最后一块没有收到人声（长度上限切块后音频立即结束）时没有可交付内容；
  // 空闲态调用同样没有内容。两种情况都只做复位。
  if (!had_enough_speech || chunk_speech_frames_ == 0) {
    reset_to_idle();
    return domain::Result<bool>::success(false);
  }
  close_chunk(SegmentCutReason::kInputEnded, out);
  reset_to_idle();
  return domain::Result<bool>::success(true);
}

std::size_t SpeechSegmenter::abandon() {
  const std::size_t discarded_speech_frames = segment_speech_frames_;
  reset_to_idle();
  return discarded_speech_frames;
}

const std::string& SpeechSegmenter::stream_id() const noexcept {
  return stream_id_;
}

const SpeechSegmentationConfig& SpeechSegmenter::config() const noexcept {
  return config_;
}

std::uint64_t SpeechSegmenter::frames_fed() const noexcept {
  return frames_fed_;
}

std::size_t SpeechSegmenter::delivered_chunks() const noexcept {
  return delivered_chunks_;
}

std::size_t SpeechSegmenter::dropped_short_segments() const noexcept {
  return dropped_short_segments_;
}

std::size_t SpeechSegmenter::max_speech_cuts() const noexcept {
  return max_speech_cuts_;
}

std::size_t SpeechSegmenter::max_chunk_cuts() const noexcept {
  return max_chunk_cuts_;
}

std::size_t SpeechSegmenter::preroll_frames_held() const noexcept {
  return preroll_.size();
}

std::size_t SpeechSegmenter::pending_silence_frames() const noexcept {
  return pending_silence_.size();
}

std::size_t SpeechSegmenter::chunk_frames_held() const noexcept {
  return chunk_frames_.size();
}

bool SpeechSegmenter::in_speech() const noexcept {
  return in_speech_;
}

void SpeechSegmenter::begin_segment(std::uint64_t speech_sequence, SegmenterStep& step) {
  in_speech_ = true;
  ++next_segment_id_;
  chunk_index_ = 0;
  chunk_frames_.clear();
  chunk_speech_frames_ = 0;
  segment_speech_frames_ = 0;
  silence_run_ = 0;
  pending_silence_.clear();
  // 段头前置缓冲：起音之前的静音一并交给识别，避免把声母切掉。
  // 代价是每段开头多出最多 preroll_frames 帧静音，这对识别是可接受的输入冗余。
  const std::uint64_t held_frames = static_cast<std::uint64_t>(preroll_.size());
  chunk_frames_.assign(preroll_.begin(), preroll_.end());
  preroll_.clear();
  // 段起点回推前置缓冲长度。无符号减法不可能下溢，因为帧序号从 0 开始且
  // 前置缓冲只包含已经喂入的帧；三元判断只是把这条不变量写成显式代码。
  start_sequence_ = speech_sequence >= held_frames ? speech_sequence - held_frames : 0;
  step.speech_started = true;
  step.speech_sequence = speech_sequence;
  step.start_sequence = start_sequence_;
  step.segment_id = next_segment_id_;
}

void SpeechSegmenter::start_continuation_chunk() {
  ++chunk_index_;
  chunk_frames_.clear();
  chunk_speech_frames_ = 0;
  // 续块不再带段头前置缓冲：说话并未结束，再补一段静音只会让识别看到假的起音前静音。
}

void SpeechSegmenter::flush_pending_silence_into_chunk() {
  for (const auto& held : pending_silence_) {
    chunk_frames_.push_back(held);
  }
  pending_silence_.clear();
}

void SpeechSegmenter::close_chunk(SegmentCutReason reason, SpeechSegment& out) {
  out.stream_id = stream_id_;
  out.segment_id = next_segment_id_;
  out.chunk_index = chunk_index_;
  out.start_sequence = start_sequence_;
  out.speech_frames = chunk_speech_frames_;
  out.cut_reason = reason;
  out.frames = std::move(chunk_frames_);
  // move 之后容器处于有效但未指定状态；显式清空，使下一块的起始状态不依赖标准库实现。
  chunk_frames_.clear();
  // 只清块内人声计数：整段人声计数继续累计，min_speech_frames 判定的是整次说话。
  chunk_speech_frames_ = 0;
  ++delivered_chunks_;
}

void SpeechSegmenter::reset_to_idle() {
  in_speech_ = false;
  preroll_.clear();
  pending_silence_.clear();
  chunk_frames_.clear();
  chunk_speech_frames_ = 0;
  segment_speech_frames_ = 0;
  chunk_index_ = 0;
  start_sequence_ = 0;
  silence_run_ = 0;
  // frames_fed_ 与 next_segment_id_ 故意不复位：输入流序号和段编号必须跨段单调，
  // 否则“取消后的新段”会复用旧编号，归属证据失去区分度。
}

void SpeechSegmenter::push_preroll(const domain::AudioFrame& frame) {
  if (config_.preroll_frames == 0) {
    return;
  }
  preroll_.push_back(frame);
  while (preroll_.size() > config_.preroll_frames) {
    // 超限时丢弃最旧静音帧：段头只需要紧邻起音的那一段静音，更早的静音不含用户信息。
    preroll_.pop_front();
  }
}

}  // namespace nexweave::runtime

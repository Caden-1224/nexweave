// NexWeave 语音活动判定与语音分段契约。
//
// 职责：把“这一帧是不是人声”和“一次连续说话从哪里开始、到哪里结束”拆成两层。
//   - ISpeechActivityDetector 只回答单帧问题，是接入真实 VAD（Voice Activity
//     Detection，语音活动检测）的接缝；本版由固定活动脚本实现。
//   - SpeechSegmenter 只根据逐帧判定维护分段状态机、前置缓冲和容量上限，不读波形、
//     不访问设备或模型、不使用真实时间。
// 这样分层的理由：分段的正确性（起止、静音超时、最长语音切块、短脉冲丢弃）只由
// 帧顺序与判定序列决定，因此可以在毫秒内用固定脚本复现；替换判定来源不会改变
// 分段语义，接入真实 VAD 时只需替换判定器。
//
// 输入前提：调用方必须从常驻输入流的第一帧开始，按顺序、不跳帧地把每一帧连同该帧的
// 判定结果交给 SpeechSegmenter。分段器内部的帧计数就是输入流序号，跳帧会让
// SpeechSegment::start_sequence 失去“输入流内序号”的含义。
//
// 并发：本文件的类型不含线程、条件变量与真实时钟，同一实例由调用方串行调用，
// 不提供内部同步。
// 资源：不创建文件、socket、线程或设备句柄；帧副本由标准库容器拥有，析构即释放，
// 分配失败按标准库异常向上传播，不做部分交付。
// 版本：本契约是 v1。新增判定来源或段字段必须保持既有字段语义不变；字段的单位统一
// 是“帧”（1 帧 = 320 样本 = 20 ms），不使用毫秒或采样数，避免单位混用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "../domain/audio_frame.hpp"
#include "../domain/error.hpp"

namespace nexweave::runtime {

// 单帧语音活动判定结果。kSilence 表示该帧不含人声，kSpeech 表示含人声。
// 判定只描述“这一帧”，不代表整段结论；分段结论由 SpeechSegmenter 依据判定序列给出。
enum class SpeechActivity : std::uint8_t {
  kSilence,
  kSpeech
};

// 逐帧语音活动判定接缝。
//
// 调用约定：detect 必须由调用方串行调用，必须无阻塞、无睡眠、无真实时间依赖；
// 判定器可以在帧之间保留内部状态（真实 VAD 的循环状态），因此跨语音段不得复位，
// 只有跨输入流或显式 reset() 才复位。复位会丢弃全部历史，复位后第一帧的判定与
// 流的首帧等价。
//
// 错误语义：返回 kDeviceFailure/kBackendFailure 表示判定不可用。调用方必须据此结束
// 当前段并上报失败，绝不允许把判定失败当成静音——那会把用户正在说的话静默截断，
// 而且失败原因会从证据里消失。
//
// 本版边界：本版只有确定性活动脚本实现，它按脚本给出判定，不读波形、不使用模型，
// 因此不声称“识别”了音频内容，也不产生判定错误。真实 VAD 属于后续任务，它接在这个
// 接缝上，必须自行声明模型加载、推理线程、失败与取消语义。
class ISpeechActivityDetector {
 public:
  virtual ~ISpeechActivityDetector() = default;

  // 判定一帧是否含人声。
  // 输入前提：frame 满足统一音频契约（16 kHz/单声道/S16_LE/320 样本）。
  // 输出后置：成功时 value 为该帧判定，且本调用不改变调用方数据；失败时不得
  // 消费 value，判定器的内部状态不保证前进。
  virtual domain::Result<SpeechActivity> detect(const domain::AudioFrame& frame) = 0;

  // 复位判定器内部状态（跨输入流或显式重放时使用）；幂等，不抛异常。
  virtual void reset() = 0;
};

// 固定活动脚本的一段：连续 frames 帧都取同一个活动判定。
// frames 为 0 的段被忽略；脚本不读波形，因此不声称“识别”了音频内容。
struct ActivityRun {
  std::size_t frames = 0;
  SpeechActivity activity = SpeechActivity::kSilence;
};

// 确定性活动脚本：把“第 n 帧到第 m 帧是人声”写成显式段列表，用于在无模型、无真实
// 时间的条件下复现起音、结束、静音、短脉冲和最长语音。
// 生命周期：构造时按值接管段列表，构造后不再修改；reset() 回到脚本开头，因此同一
// 脚本可以跨多轮输入重复使用。脚本耗尽后保持最后一段的判定，保证长输入不会出现
// “越界后突然改变活动”的隐式翻转。对象不拥有外部资源，可在栈上使用。
class SpeechActivityScript final : public ISpeechActivityDetector {
 public:
  // 空脚本等价于“全部静音”：判定始终返回 kSilence，不会伪造人声。
  explicit SpeechActivityScript(std::vector<ActivityRun> runs);

  // 返回当前帧的判定并推进游标。本实现不使用模型与设备，因此永远返回成功；
  // 游标在脚本末尾饱和，不越界、不回绕。
  domain::Result<SpeechActivity> detect(const domain::AudioFrame& frame) override;

  // 回到脚本开头并清零已消费帧数；幂等，不释放外部资源。
  void reset() override;

  // 脚本是否已经耗尽（后续判定保持最后一段的活动）。判定包含“最后一段的剩余帧数”：
  // 恰好读完声明帧数时即视为耗尽，而不是要等到再调用一次 detect 才推进游标。
  bool exhausted() const noexcept;
  // 已消费的帧数快照；用于测试核对脚本被完整使用。
  std::size_t frames_consumed() const noexcept;

 private:
  std::vector<ActivityRun> runs_;
  std::size_t run_index_ = 0;
  std::size_t used_in_run_ = 0;
  std::size_t frames_consumed_ = 0;
};

// 段块被切出并交付的原因。取消、放弃和失败都不产生段，因此不在本枚举内。
enum class SegmentCutReason : std::uint8_t {
  // 人声结束后连续静音达到 min_silence_frames，属于正常的一次说话结束。
  kSilenceTimeout,
  // 块内人声帧数达到 max_speech_frames，同一次说话被切成多块。
  kMaxSpeech,
  // 块内总帧数达到 chunk_frame_capacity()，同一次说话被切成多块。
  kMaxChunkFrames,
  // 输入正常结束时的冲刷。
  kInputEnded
};

// 一次连续说话的音频与归属。
//
// 归属三要素（新段开头必须可追溯）：stream_id 说明属于哪条常驻输入流；
// segment_id 说明是这条流里的第几次说话（从 1 开始单调递增）；chunk_index 说明是
// 该次说话的第几块。因长度上限被切成多块时，各块共享同一 segment_id，按
// chunk_index 顺序拼接即得到完整一次说话；只有 chunk_index==0 的块代表“说话开始”，
// 因此只有首块会产生起音通知，续块不会被误判成新的打断。
//
// 音频范围：frames 按时间顺序排列，不含非法帧。段头包含最多 preroll_frames 帧起音前静音
// （避免切掉声母），段尾静音已裁掉（它不含信息量，保留只会增加识别开销）。
// speech_frames 是该块内的人声帧数，不含段头静音。
//
// start_sequence 的含义必须按“次”而不是按“块”理解：它是**本次说话第一帧**（含段头前置
// 缓冲）在输入流内的序号，同一 segment_id 的所有块共享同一个值，因此它回答的是“这次说话从
// 哪里开始”，而不是“本块从哪里开始”。要还原块的音频位置，请用 chunk_index 排序并用各块的
// frames 拼接；块与块之间可能缺少最多 min_silence_frames-1 帧停顿（因长度上限切块时被丢弃），
// 因此块的帧序号不保证连续，本契约也不承诺按序号无损重建。
// 所有权：frames 是该段的独立副本，源帧可立即释放；接收方拥有整段内存。
struct SpeechSegment {
  std::string stream_id;
  std::uint64_t segment_id = 0;
  std::uint64_t chunk_index = 0;
  std::uint64_t start_sequence = 0;
  std::size_t speech_frames = 0;
  SegmentCutReason cut_reason = SegmentCutReason::kSilenceTimeout;
  std::vector<domain::AudioFrame> frames;
};

// 新语音起音通知：一次说话首块开始时的归属快照。
// 它只描述“用户开始说话了”，不携带音频；音频仍留在产生它的输入对象里等待被取走，
// 因此“打断旧回答”与“保留新语音开头”是两件互不覆盖的事。
struct SpeechStartNotice {
  std::string stream_id;
  std::uint64_t segment_id = 0;
  std::uint64_t start_sequence = 0;
  std::uint64_t speech_sequence = 0;
};

// 打断监视接缝：会话在播放交付边界只读消费一次“用户是否已经开始说话”。
//
// 调用约定：take_speech_started 不得阻塞、不得睡眠、不得回调会话；同一实例可被
// 输入拥有者线程与会话线程并发调用，实现负责内部同步。
// 消费语义：每次调用取走一条未消费的最早通知；没有通知时返回 nullopt。
// 同一段说话最多产生一条通知（因长度上限切出的续块不产生通知），但两次不同的说话
// 在未被消费时会合并成一条最早的记录，保证“打断”这个事实不会因为消费不及时而丢失。
class IBargeInMonitor {
 public:
  virtual ~IBargeInMonitor() = default;

  // 取走最早的一条未消费起音通知；没有则返回 nullopt。
  virtual std::optional<SpeechStartNotice> take_speech_started() = 0;
};

// 把段内帧按顺序拼成连续 PCM 样本（独立副本），供 IAsr 逐帧入口或证据落盘使用。
// 输入前提：frames 中每一帧都应当满足统一音频契约；不满足时跳过该帧而不是返回半截
// 数据，空段返回空向量。本函数不修改输入段。
std::vector<std::int16_t> segment_samples(const SpeechSegment& segment);

// 分段策略。所有容量单位都是“帧”，换算关系为 1 帧 = 20 ms = 320 样本。
// 归一化规则（构造时执行，避免非法值产生畸形分段）：
//   - min_speech_frames 为 0 时抬高到 1；
//   - min_silence_frames 为 0 时抬高到 1；
//   - max_speech_frames 小于 min_speech_frames 时抬高到 min_speech_frames；
//   - preroll_frames 允许为 0，表示段头不带静音。
struct SpeechSegmentationConfig {
  // 段头前置缓冲上限：保留起音前最近这么多帧静音。取 0 表示段从起音帧开始。
  std::size_t preroll_frames = 15;  // 15 帧 = 300 ms
  // 连续静音帧数达到该值即判定一次说话结束（静音超时）。
  std::size_t min_silence_frames = 25;  // 25 帧 = 500 ms
  // 整段人声帧数少于该值的段不交付：短脉冲（咳嗽、碰撞）不是一次说话。
  // 判定针对整次说话的人声帧总数，而不是单个块，因此切块不会让长说话被误丢。
  std::size_t min_speech_frames = 13;  // 13 帧 = 260 ms
  // 块内人声帧数达到该值即强制切块，防止一次说话无限增长。
  std::size_t max_speech_frames = 1000;  // 1000 帧 = 20 s
};

// 单帧推进的可观察结果。字段互相独立：一次调用可能同时“完成上一段”并“开始新一段”
// （静音超时后紧接着起音），因此不能压缩成单一枚举。
struct SegmenterStep {
  // 本次调用是否完成并交付了一个段块；交付内容写入 push 的 out 参数。
  bool produced_segment = false;
  // 本次调用是否确认了一次新的说话起音。只有“从静音进入人声且属于本次说话首块”时为真；
  // 因长度上限切出的续块不产生起音，否则设备会把同一次说话误判成两次打断。
  bool speech_started = false;
  // 起音帧在输入流内的序号；speech_started 为假时无意义。
  std::uint64_t speech_sequence = 0;
  // 本次起音所属段的第一帧（含段头前置缓冲）在输入流内的序号。
  std::uint64_t start_sequence = 0;
  // 本次起音所属的段编号（从 1 开始，一次连续说话共享同一编号）。
  std::uint64_t segment_id = 0;
};

// 确定性语音分段器：把“帧 + 判定”序列变成带归属的语音段。
//
// 状态机只有两个状态（空闲、说话中），迁移由判定序列驱动：
//   空闲 --(人声帧)--> 说话中     同时用前置缓冲做段头并产生起音通知
//   说话中 --(人声帧)--> 说话中   块内停顿先落地，再追加本帧；达到上限即切块
//   说话中 --(连续静音达上限)--> 空闲   交付（或丢弃过短段）并裁掉段尾静音
// 顺序理由：静音帧先进入待定缓冲、只有后续人声帧到来才落地，是为了让“说话结束”的
// 尾随静音被裁掉，而“说话中间的停顿”被保留。若反过来一边收一边追加，就无法区分
// 两者：要么把停顿切碎成多段，要么把尾音静音塞给识别。
//
// 容量不变量（每次 push 返回后成立）：
//   - 前置缓冲帧数 <= preroll_frames；
//   - 待定停顿帧数 <= min_silence_frames - 1（达到 min_silence_frames 立即结束说话）；
//   - 当前块帧数 <= chunk_frame_capacity()。
// 单块内存因此与输入长度无关。
//
// 错误语义：非法帧返回 kInvalidInput 且不改变任何状态（不计数、不入缓冲、不推进序号），
// 调用方可以修正后重试同一帧。本类不产生设备或后端错误。
// 分配语义：交付段时复制帧可能因内存不足抛出 std::bad_alloc。抛出时不会有段被交付，
// 但帧序号与缓冲可能已经推进；调用方应把这条输入视为不可继续并走停止路径，而不是假定
// 状态未变后重试同一帧。
// 资源：段内帧全部是独立副本；abandon() 与析构直接释放，不涉及外部句柄。
class SpeechSegmenter final {
 public:
  // stream_id 是这条常驻输入流的身份，会被写入每个交付的段，保证“新段开头有归属”。
  // 构造只做配置归一化，不分配外部资源。越界配置按 SpeechSegmentationConfig 的
  // 归一化规则处理，构造函数本身不会失败。
  explicit SpeechSegmenter(std::string stream_id, SpeechSegmentationConfig config = {});

  // 追加一帧及其判定结果。
  // 输入前提：frame 满足统一音频契约，且必须按输入流顺序、不跳帧地调用。
  // 输出后置：成功时返回本帧的推进结果；produced_segment 为真时 out 被整体覆盖为
  // 新完成的段块（含独立帧副本），调用方可立即复用传入帧。
  // 错误语义：非法帧返回 kInvalidInput，且不改变任何分段状态。
  domain::Result<SegmenterStep> push(const domain::AudioFrame& frame, SpeechActivity activity,
                                     SpeechSegment& out);

  // 输入自然结束时的冲刷：把正在进行的块按整段人声帧数判定后交付或丢弃。
  // 输出后置：交付时写入 out、返回 true，且 cut_reason 为 kInputEnded；丢弃或无事
  // 可做时返回 false 且不写 out。幂等：冲刷后回到空闲态，重复调用返回 false；
  // 之前因长度上限已经交付的块不会重复交付。
  domain::Result<bool> flush(SpeechSegment& out);

  // 放弃正在进行的块（显式停止、设备失败等“结尾不可知”的路径）：不交付任何段。
  // 返回被丢弃的人声帧数（不含段头静音与待定停顿），供调用方计数与告警；幂等。
  // 之所以丢弃而不是冲刷：这些路径下音频是否连续、说话是否说完都无法确认，
  // 把截断的音频当成完整一次说话交付，会让识别结果与证据同时失真。
  std::size_t abandon();

  // ---- 只读观测：不改变分段状态，供测试与运行证据核对 ----

  const std::string& stream_id() const noexcept;
  const SpeechSegmentationConfig& config() const noexcept;
  // 已接受的帧数，等于下一帧的输入流序号。
  std::uint64_t frames_fed() const noexcept;
  // 已交付的段块总数（含续块）。
  std::size_t delivered_chunks() const noexcept;
  // 因整段人声不足 min_speech_frames 被丢弃的说话次数（整段丢弃，不含已交付块）。
  std::size_t dropped_short_segments() const noexcept;
  // 因块内人声帧数达到上限而切块的次数。
  std::size_t max_speech_cuts() const noexcept;
  // 因块内总帧数达到上限而切块的次数。
  std::size_t max_chunk_cuts() const noexcept;
  // 当前前置缓冲持有的帧数（<= preroll_frames）。
  std::size_t preroll_frames_held() const noexcept;
  // 当前待定停顿帧数（<= min_silence_frames - 1）。
  std::size_t pending_silence_frames() const noexcept;
  // 当前块持有的帧数（<= chunk_frame_capacity()）。
  std::size_t chunk_frames_held() const noexcept;
  // 是否存在正在进行的说话。
  bool in_speech() const noexcept;
  // 单块帧数硬上限。由配置推导，不是一个独立旋钮：
  //   preroll_frames + max_speech_frames + min_silence_frames
  // 段头前置缓冲计入上限，因为首块在追加起音帧之前就已经装了整段前置缓冲。
  // 两个长度上限必须同时存在：只限制人声帧数无法约束“人声帧之间反复夹接近上限的
  // 停顿”这种输入，那种输入会让单块线性增长。默认值 15+1000+25 = 1040 帧
  // （20.8 s、约 666 KB），因此单块内存与输入长度无关。
  std::size_t chunk_frame_capacity() const noexcept;

 private:
  // 进入说话态：用前置缓冲做段头、分配段编号并填写起音信息。
  void begin_segment(std::uint64_t speech_sequence, SegmenterStep& step);
  // 因长度上限切块后开始同一段说话的下一块：沿用段编号与段起点，不再产生起音。
  void start_continuation_chunk();
  // 把待定停顿落地到当前块：只有确认说话继续时才调用，否则这些帧会被裁掉。
  void flush_pending_silence_into_chunk();
  // 收尾交付：把当前块写入 out、递增交付计数并清空块状态。
  void close_chunk(SegmentCutReason reason, SpeechSegment& out);
  // 回到空闲态并清空全部缓冲；段编号水位保留，保证编号跨段单调。
  void reset_to_idle();
  // 空闲态收集段头前置缓冲：超过上限时丢弃最旧静音帧（静音不含用户信息）。
  void push_preroll(const domain::AudioFrame& frame);

  std::string stream_id_;
  SpeechSegmentationConfig config_;
  // 段头前置缓冲：环形保留最近 preroll_frames 帧静音。
  std::deque<domain::AudioFrame> preroll_;
  // 段内待定停顿：说话尚未结束时暂存，确认继续后落地，判定结束则裁掉。
  std::deque<domain::AudioFrame> pending_silence_;
  // 当前块：块内帧与其中的人声帧数分别计数，因为两个上限的判据不同。
  std::vector<domain::AudioFrame> chunk_frames_;
  std::size_t chunk_speech_frames_ = 0;
  // 本次说话累计人声帧数：min_speech_frames 的判据是整次说话而不是单块。
  std::size_t segment_speech_frames_ = 0;
  std::size_t silence_run_ = 0;
  std::uint64_t frames_fed_ = 0;
  std::uint64_t next_segment_id_ = 0;
  std::uint64_t chunk_index_ = 0;
  std::uint64_t start_sequence_ = 0;
  bool in_speech_ = false;
  std::size_t delivered_chunks_ = 0;
  std::size_t dropped_short_segments_ = 0;
  std::size_t max_speech_cuts_ = 0;
  std::size_t max_chunk_cuts_ = 0;
};

}  // namespace nexweave::runtime

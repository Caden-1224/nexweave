// NexWeave 常驻音频输入：一条音频输入流的唯一拥有者。
//
// 职责：把“从音频源持续读帧”与“会话按轮次消费语音段”解耦。它拥有采集生命周期
// （开始、持续生产、正常结束、显式停止），并保证该生命周期跨越多轮回答：一轮回答
// 正常收尾或被取消只作废那一轮的输出代际，既不关闭采集，也不清空已经攒下的新语音。
//
// 与分段契约的分工：本类不做人声判断，也不解析波形。逐帧活动判定由注入的
// ISpeechActivityDetector 给出，分段状态机由 SpeechSegmenter 维护，本类只负责
// 读帧、推进判定与分段、把完成的段放进有界等待队列，并把起音事件暴露给会话。
//
// 单一拥有者不变量：一个实例绑定恰好一个 capability::IAudioSource，构造后无法替换。
// 同一输入流只允许一个拥有者，因此“谁拥有麦克风”不需要运行时仲裁；需要切换输入源
// 时由外层重建实例，而不是在同一实例上并存两个源。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include "../capability/backend.hpp"
#include "speech_segmentation.hpp"

namespace nexweave::runtime {

// 常驻输入的容量策略。所有容量单位都是“帧”或“段”，不使用字节或毫秒，避免单位混用。
struct ResidentAudioInputConfig {
  // 语音分段策略（前置缓冲、静音超时、最短/最长语音）。
  SpeechSegmentationConfig segmentation{};
  // 已完成但尚未被会话取走的语音段上限（段）。取 0 表示不允许排队：完成的段会被
  // 立即丢弃并计数，用于“只关心当前这一句”的受限配置。
  std::size_t pending_segment_capacity = 4;
};

// 取段结果的四态。用独立枚举而不是“有值/无值”二元结果，是为了不让“暂时还没有”
// 和“已经结束”混为一谈——前者应当继续泵帧，后者应当结束循环。
enum class SegmentAvailability : std::uint8_t {
  kAvailable,  // segment 有效
  kPending,    // 输入仍在采集，但当前没有已完成的段
  kEnded,      // 输入已经结束或已停止，且队列已空，不会再有新段
  kTimeout     // 阻塞等待在超时前没有等到段，输入仍在采集
};

// 取段结果。只有 availability==kAvailable 时 segment 有效。
struct SegmentTakeResult {
  SegmentAvailability availability = SegmentAvailability::kPending;
  SpeechSegment segment;
};

// 常驻音频输入实现。线程模型与资源归属：
//   - 生产者侧（start/pump/end_input/stop）由输入拥有者线程串行调用；同一实例同时
//     只允许一个 pump 调用者，因为判定器与分段器不含内部同步。
//   - 消费者侧（take_segment/wait_for_segment/take_speech_started 与全部只读观测）
//     可以由另一个线程调用。队列、终态与计数由互斥量保护，等待用条件变量唤醒。
//   - 读设备期间不持锁：真实声卡的 read 可能阻塞到它自己声明的超时，持锁会让
//     stop() 一直等在这次读上，取消无法及时生效。
// 资源：不拥有线程、定时器、文件或 socket；借用注入的源与判定器，因此两者必须比本
// 对象活得久。唯一自有资源是帧副本构成的缓冲与队列，析构即释放。
// 失败收敛：源读失败或判定失败都会结束采集、唤醒等待者并保留结构化错误；未完成的
// 说话按“结尾不可知”放弃而不是当完整一句交付。所有失败路径都不吞掉原因。
class ResidentAudioInput final : public IBargeInMonitor {
 public:
  // stream_id 是这条输入流的身份（必须非空），会写入每个交付段与起音通知。
  // 借用 source 与 detector，二者必须比本对象活得久；构造不打开设备、不读帧、
  // 不创建线程，只做配置拷贝与分段器构造。
  ResidentAudioInput(capability::IAudioSource& source, ISpeechActivityDetector& detector,
                     std::string stream_id, ResidentAudioInputConfig config = {});

  ResidentAudioInput(const ResidentAudioInput&) = delete;
  ResidentAudioInput& operator=(const ResidentAudioInput&) = delete;

  // 开始常驻采集：打开音频源并复位判定器。每个实例只允许一次成功的 start()，
  // 重复调用返回 kAlreadyCompleted；流标识为空或源打开失败返回对应错误且不改变状态。
  // 打开失败后可以修正环境重试，因此占位状态在失败路径被回滚。
  domain::OperationResult start();

  // 泵入最多 max_frames 帧：逐帧读源、判定活动、推进分段，并把完成的段与起音通知
  // 发布给消费者。返回本次实际读取的帧数。
  // 语义：预算用尽返回成功；源自然读完时冲刷未完成段、结束采集并返回成功（帧数可能
  // 小于预算）；已经结束或已停止时返回成功 0 帧，便于“读到 0 就退出”的循环收尾；
  // 源或判定失败时结束采集、唤醒等待者并返回该错误。未调用 start() 返回 kInvalidInput。
  domain::Result<std::size_t> pump(std::size_t max_frames);

  // 非阻塞取下一个已完成段。队列为空时区分“仍在采集”（kPending）与“已经结束或
  // 已停止”（kEnded）。调用不改变采集状态。
  // 线程安全：由互斥量线性化，可由消费者线程与输入拥有者线程并发调用。
  SegmentTakeResult take_segment();

  // 阻塞等待下一个已完成段，直到取到段、或输入结束/停止。
  // 输入结束或停止会立即唤醒等待者并返回 kEnded，因此“输入停止可以唤醒阻塞等待”是
  // 本接口的硬保证，而不是靠超时兜底；等待期间互斥量被释放，不阻塞生产者。
  // 这是消费循环的首选阻塞入口：它没有超时分支，也不依赖定时等待实现。
  // 线程安全：由互斥量线性化，可由消费者线程调用；等待期间不持有锁。
  SegmentTakeResult wait_for_segment();

  // 带超时的阻塞等待：语义与无超时重载完全一致，只是等待在 timeout_ms 之后结束。
  // 超时且既没有新段也没有终态时返回 kTimeout，调用方可据此决定继续泵帧还是收尾，
  // 避免设备既不产出也不结束时消费方永久阻塞。
  // 两个重载共用同一把锁、同一组状态和同一个谓词，差异只在等待调用是否有截止时间。
  // 线程安全：同无超时重载。
  SegmentTakeResult wait_for_segment(std::size_t timeout_ms);

  // 输入自然结束：冲刷未完成段（满足最短语音才交付）、关闭音频源、唤醒等待者。
  // 已经攒下的段仍可继续取走，直到取空后返回 kEnded。
  // 幂等性：每个实例只允许一次成功的结束，重复调用（含源已经自然读完的情况）返回
  // kAlreadyCompleted，并且不会重复冲刷或重复交付。
  domain::OperationResult end_input();

  // 显式停止（进程退出、设备不可用，或输入拥有者决定放弃这批音频）：放弃正在进行的说话、
  // 丢弃尚未被取走的等待段与未消费的起音通知、关闭音频源并唤醒等待者。
  // 本方法只属于输入拥有者的收尾，与“取消一次回答”无关：回答取消走会话的取消路径，
  // 不调用本方法，因此打断旧回答不会删除新语音的开头。
  // 幂等且不抛异常：重复调用返回成功且不改变已经记录的计数。
  // 与 end_input 的区别是语义而非顺序：正常结束保留可消费结果，显式停止丢弃它们，
  // 因为停止发生在“这批音频是否还算数”无法确认的时刻。
  domain::OperationResult stop() noexcept;

  // 采集是否仍然存活。回答轮次结束、回答被取消都不会改变它；只有正常结束、显式停止
  // 或源/判定失败会把它置为假。
  bool capturing() const;

  // 结束采集时保留的结构化错误（正常结束与仍在采集时为空错误）。
  domain::Error last_error() const;

  // ---- IBargeInMonitor ----
  // 取走最早的一条未消费起音通知；未消费时多条通知会合并成最早的一条并计数。
  // 线程安全：由互斥量线性化，可由会话线程与输入拥有者线程并发调用。
  std::optional<SpeechStartNotice> take_speech_started() override;

  // ---- 只读观测：供测试与运行证据核对，不改变任何状态 ----
  // 以下接口都只读取互斥量保护下的快照，因此可以由消费者线程与输入拥有者线程并发调用；
  // 使用 const 与互斥量而非常量成员，正是为了让只读观测也不需要调用方额外同步。

  const std::string& stream_id() const noexcept;
  // 已经泵入的总帧数（等于分段器已接受的帧数）。
  std::uint64_t frames_pumped() const;
  // 已经交付到等待队列的段块累计数，含之后因队列溢出被丢弃的段；上限为 0（不允许排队）
  // 时没有入队，因此不计入。它与 pending_segments()（当前仍在队列里的段数）和
  // dropped_segments()（被丢弃的段数）一起构成“产出—积压—丢弃”的完整账目。
  std::size_t queued_segments() const;
  // 因等待队列溢出被丢弃的最旧段数，加上显式停止时丢弃的未取走段数。
  std::size_t dropped_segments() const;
  // 因整段人声不足下限被丢弃的说话次数（转发自分段器）。
  std::size_t dropped_short_segments() const;
  // 因停止或失败被放弃的未完成说话次数。
  std::size_t abandoned_segments() const;
  // 被放弃的未完成说话中的人声帧数合计。
  std::size_t abandoned_speech_frames() const;
  // 起音通知因未被及时消费而合并的次数。
  std::size_t merged_speech_starts() const;
  // 当前等待被取走的段数。
  std::size_t pending_segments() const;

 private:
  // 取段内部实现，要求调用方已持有 mutex_。
  SegmentTakeResult take_locked();
  // 把分段器的计数复制到互斥量保护下的快照字段，要求调用方已持有 mutex_。
  // 分段器本身只由输入拥有者线程访问，其它线程只能读这些快照，因此不需要为它单独加锁。
  void sync_segmenter_counters_locked();
  // 记录一次“放弃在途说话”的计数，要求调用方已持有 mutex_；人声帧数为 0 时不计。
  void record_abandon_locked(std::size_t discarded_speech_frames);
  // 收尾公共尾部：在锁外关闭音频源并唤醒全部等待者；终态必须已经在锁内置好。
  void finish_capture(bool must_close);
  // 入队一个已完成段并执行溢出策略，要求调用方已持有 mutex_；会唤醒等待者。
  void enqueue_segment_locked(SpeechSegment segment);
  // 发布起音通知，要求调用方已持有 mutex_。
  void publish_speech_start_locked(const SegmenterStep& step);
  // 正常结束的公共尾部：冲刷分段器、关闭源、置终态并唤醒等待者。
  void finalize_natural_end();
  // 失败收敛的公共尾部：放弃未完成段、记录错误、关闭源、置终态并唤醒等待者。
  void finalize_failure(const domain::Error& error);
  // 源读取失败的统一处理：EOF 视为正常结束，其它错误视为需要放弃在途说话的失败。
  domain::Result<std::size_t> handle_source_error(const domain::Error& error,
                                                  std::size_t read_frames);
  // 置终态并唤醒等待者，要求调用方已持有 mutex_。返回“是否需要关闭音频源”，
  // 因为关闭可能等待工作线程退出，必须在锁外执行。
  bool close_capture_locked();
  // 在锁外关闭音频源；关闭失败只在尚无更早错误时记入 last_error_。
  void close_source(bool must_close);

  capability::IAudioSource& source_;
  ISpeechActivityDetector& detector_;
  std::string stream_id_;
  ResidentAudioInputConfig config_;
  // 分段器与本对象共享同一个流标识，保证段与起音通知的归属只有一个来源。
  // 声明顺序在 stream_id_ 与 config_ 之后，构造顺序因此是安全的。
  SpeechSegmenter segmenter_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<SpeechSegment> pending_;
  std::optional<SpeechStartNotice> pending_start_;
  domain::Error last_error_{};
  bool started_ = false;
  bool capturing_ = false;
  bool terminal_ = false;

  std::uint64_t frames_pumped_ = 0;
  std::size_t queued_segments_ = 0;
  std::size_t dropped_segments_ = 0;
  // 分段器的丢弃计数由拥有者线程推进；这里保存快照，使其它线程可以用同一把锁读到
  // 一致的值，而不是直接读一个正被写入的对象。
  std::size_t dropped_short_segments_ = 0;
  std::size_t abandoned_segments_ = 0;
  std::size_t abandoned_speech_frames_ = 0;
  std::size_t merged_speech_starts_ = 0;
};

}  // namespace nexweave::runtime

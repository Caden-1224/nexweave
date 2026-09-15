// NexWeave 运行证据：把一次运行的可观察事实整理成运行清单、事件流、指标流与人类可读摘要。
//
// 它解决什么问题
// --------------
// 可观测层已经定义了运行清单、事件与指标的值对象和编解码，契约层的验收也早已完成，但仓库里
// 没有任何生产者：一次运行跑完之后，“这次跑的是哪个版本、哪份配置、哪份输入”“生成、合成、
// 播放分别在什么时候发生”“取消的四个阶段是否齐全”这些事实仍然只存在于进程内存里。本文件
// 是这些事实的唯一生产者与唯一解释处。
//
// 两类时间必须分开标注
// --------------------
//   - 单调时间族（mono_*）：由注入的单调时钟测得，起点是本次运行的 run_start。它反映真实
//     耗时，因而随主机、编译选项与负载变化，**不能**逐字节复现，也**不能**当作设备性能。
//   - 调度步数族（step_*）：由记录器按提交顺序分配，与主机和墙钟无关。确定性夹具的因果
//     顺序因此可以用“步”核对，而不必依赖时间戳。
// 把两者混成一个字段会让“可复现”与“实测”互相污染：前者被读成假的性能数据，后者被读成
// 不稳定证据。族前缀是发布契约的一部分，指标名的起点与终点在实现文件里逐条给出。
//
// 未测量与零必须分开
// ------------------
// 某个里程碑没有发生时，对应指标**不产生**，而不是写 0：0 是合法的耗时（同一线性化点上
// 提交的两个阶段），把它与“没有发生”混在一起，缺失的时间点就会在汇总里看不出来。汇总的
// 里程碑表为每一个候选里程碑给出“已记录”或“未测量（原因）”，因此缺失是可读的。
//
// 事件与指标的关系
// ----------------
// 两者都从同一批里程碑记录派生，不各自维护一份事实：事件按提交顺序输出，指标是它们的时间
// 与步数投影，汇总表再投影一次。一处记录、三处渲染，三者不可能互相矛盾。
//
// 资源与线程
// ----------
// 本层不打开文件、不创建线程、不访问设备；唯一的时钟读取出自注入的单调时钟。记录入口可以
// 从任意线程调用（内部互斥量保证步数分配与容器追加的原子性），渲染入口在读侧，允许与记录
// 并发。分配失败抛 std::bad_alloc；其余失败以“空产物”表达，不伪造证据。
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../capability/generation_probe.hpp"
#include "../runtime/interaction_contract.hpp"
#include "observability.hpp"

namespace nexweave::observability {

// 里程碑名称。它是发布契约：历史证据按名称关联，已发布的名称为稳定标识，不得改动。
// 命名一律用小写下划线，取自“事件发生的事实”，而不是某次实现的内部函数名。
namespace milestone {
// 本次运行的起点与终点。起点是记录器构造时刻，终点是 finish() 的时刻。
constexpr char kRunStart[] = "run_start";
constexpr char kRunEnd[] = "run_end";
// 会话侧：新回答代际开始。
constexpr char kGenerationStarted[] = "generation_started";
// 生成侧：第一个 token 交付给接收方。只有经过生成后端的路径才会出现。
constexpr char kFirstToken[] = "first_token";
// 会话侧：第一帧 PCM 成功交付给播放组件。它是“PCM 从合成侧进入播放侧”的线性化点。
constexpr char kFirstPcm[] = "first_pcm";
// 设备侧：第一帧被播放组件真正写到音频汇。它与 kFirstPcm 是两个不同所有者的边界，
// 因此是两条记录：只有音频汇知道帧是否真的写出去了。
constexpr char kPlaybackStart[] = "playback_start";
// 三个局部完成：文本定稿、合成结束、播放结束。三者互不替代。
constexpr char kGenerationDone[] = "generation_done";
constexpr char kSynthesisDone[] = "synthesis_done";
constexpr char kPlaybackDone[] = "playback_done";
// 取消五个阶段：受理、封锁旧输出、执行退出、播放清理、取消终态。
constexpr char kCancelAccepted[] = "cancel_accepted";
constexpr char kOldOutputBlocked[] = "old_output_blocked";
constexpr char kExecutionExited[] = "execution_exited";
constexpr char kPlaybackCleared[] = "playback_cleared";
constexpr char kTerminalSucceeded[] = "terminal_succeeded";
constexpr char kTerminalCancelled[] = "terminal_cancelled";
}  // namespace milestone

// 里程碑的候选全集。汇总表按它逐项给出“已记录 / 未测量”，因此缺失的候选永远可见，
// 不需要读者自己去比对本项目前有哪些里程碑。
extern const char* const kAllMilestones[];
// 候选全集的元素个数。
extern const std::size_t kAllMilestoneCount;

// 活动标记到里程碑名的映射。返回值是 milestone 命名空间里某个常量的地址，永不为空；
// 未覆盖的枚举值返回空串，调用方必须如实跳过而不是替它编一个名字——伪造的里程碑比缺失
// 更难发现。
const char* marker_milestone_name(runtime::ActivityMarker marker) noexcept;

// 单调时钟接缝：证据层只通过它取时间。约定：now_us 单调不减、不阻塞、不抛异常，且可以
// 跨线程调用。把时钟做成接缝，是为了让“实测时间”在单元测试里也能被固定成确定值，
// 从而对时间相关的分支做精确断言，而不是靠容差比较。
class IMonotonicClock {
 public:
  virtual ~IMonotonicClock() = default;
  // 自某个任意但固定起点起的微秒数。只有两次读数之差有意义，绝对值无意义。
  virtual std::int64_t now_us() const noexcept = 0;
};

// 默认实现：steady_clock 自纪元起的微秒数。steady_clock 不受系统校时影响，因此用它计算
// 的差值不会因为时间被向前调整而出现负数；本类无状态，可在任意线程调用。
class SteadyMonotonicClock final : public IMonotonicClock {
 public:
  std::int64_t now_us() const noexcept override;
};

// 设备侧播放边界的观察者：拥有音频汇的一方向记录器报告“第一帧已经写出”。
// 单独成一个接缝而不是复用会话标记，是因为两者的所有者不同：会话知道“我交付了第一帧”，
// 只有音频汇知道“帧真的写出去了”。把两条事实合并会掩盖“交付成功但设备写入失败”。
class IPlaybackBoundaryObserver {
 public:
  virtual ~IPlaybackBoundaryObserver() = default;
  // 第一帧成功写出音频汇时调用一次，由写出帧的线程同步调用。
  // 实现必须非阻塞、不抛异常；重复调用由实现自行去重。
  virtual void on_first_frame_written() = 0;
};

// 一次运行的机器与版本事实。字段与运行清单一一对应；没有该项时写 "none" 而不是空串——
// 空串会被清单校验判为缺字段，而“没有驱动”本身是一个事实。
struct RunEnvironment {
  std::string git_commit = "none";
  std::string compiler = "none";
  std::string cmake = "none";
  std::string runtime = "none";
  std::string driver = "none";
  std::string model = "none";
  std::string device = "none";
};

// 运行身份与配置事实。哈希与命令由调用方提供：它们描述“这次跑的是什么”，而本层不解释
// 调用方的配置来源，也不重新实现调用方的哈希算法（两处实现迟早会分歧）。
struct RunEvidenceConfig {
  // 运行标识。同一条命令重复运行得到同一个 run_id，因此它标识“哪一次配置”，不标识
  // “第几次执行”；区分两次执行靠清单里的日历时间。
  std::string run_id;
  std::string profile = "mock";
  // 足以复现本次运行的实际命令。
  std::string command;
  // 命令、配置、输入的关联指纹（由调用方计算）。用于把两次运行归到同一份命令/配置/输入上。
  std::string command_hash;
  std::string config_hash;
  std::string input_hash;
  // 里程碑与事件的归属。request_id 必须满足领域格式；session_id 允许为空（未绑定会话）。
  std::string request_id;
  std::string session_id;
  RunEnvironment environment;
  // 音频契约的固定帧时长（毫秒），只用于把帧数换算成“按合同应当占用的播放时长”。
  // 非正值会让该推导指标不产生，因为那说明调用方给出的音频契约不可用。
  std::int64_t frame_ms = 20;
};

// 一次运行的结论。由调用方在收尾时给出：本层不解释退出码与错误码的业务含义，只如实记录。
struct RunOutcome {
  // 进程退出码语义由调用方定义（0 表示按契约完成，含“预期内的失败”）。
  int exit_code = 1;
  // 运行级错误码的稳定文本名；正常收敛时为 "none"。
  std::string error_code = "none";
  // 场景声明的外部形态是否与实测一致。为假说明这次运行不能支持该场景的结论。
  bool expectation_matched = true;
  // 本次运行交给播放组件并由其写出音频汇的帧数。它来自会话运行记录，而不是线上事件：
  // v1 的请求入口只交付逐轮文本与终态，逐帧 PCM 下行属于后续传输任务。
  std::size_t audio_frames = 0;
};

// 一条里程碑记录：事件、指标与摘要的唯一数据来源。
struct MilestoneRecord {
  // 稳定里程碑名，取值来自上面的 milestone 命名空间。
  std::string name;
  std::string request_id;
  std::string session_id;
  std::uint64_t generation = 0;
  // 调度步数：本次运行内从 0 开始按提交顺序递增，与主机和墙钟无关。
  std::uint64_t step = 0;
  // 自 run_start 起的单调微秒。实测值，随主机与负载变化。
  std::int64_t mono_us = 0;
  // 记录来源：session（会话阶段）、device（音频汇写出）、generation（生成后端事实）、
  // runner（记录器自己的起止点）。用于区分同名的不同边界，空串表示未标注。
  std::string source;
};

// 运行证据记录器。它同时是会话标记观察者、生成进度观察者与设备侧播放边界观察者，
// 因此三类事实都汇到同一批里程碑上，而不是各自维护一条时间线。
//
// 所有权：单调时钟按借用保存，必须比本对象活得久；本对象不拥有任何外部资源。
// 生命周期：构造即本次运行的起点；finish() 之后记录入口一律丢弃新的里程碑（含并发写入
// 之后到达的迟到回调），使渲染读到的是同一份快照，不会出现“摘要里多出一条事件”。
// 线程安全：记录入口之间、记录与渲染之间都可以并发；步数分配在锁内完成，因此不会重复。
// 有界性：本对象只随里程碑数量增长。调用方在收尾时调用 finish()，之后不再增长。
class RunEvidenceRecorder final : public runtime::IMarkerObserver,
                                  public capability::IGenerationObserver,
                                  public IPlaybackBoundaryObserver {
 public:
  RunEvidenceRecorder(const RunEvidenceConfig& config, const IMonotonicClock& clock);

  RunEvidenceRecorder(const RunEvidenceRecorder&) = delete;
  RunEvidenceRecorder& operator=(const RunEvidenceRecorder&) = delete;

  // ---- 会话阶段（runtime::IMarkerObserver）----
  // 每个提交的标记映射成一个里程碑，步数按提交顺序分配。一次取消会连续提交五个阶段，
  // 它们因此是五条记录而不是一条：顺序不变量与阶段齐全性都可以被单独核对。
  void on_marker(runtime::ActivityMarker marker, std::uint64_t generation) override;

  // ---- 生成后端事实（capability::IGenerationObserver）----
  // 首次 token 交付记为首个 first_token；后续 token 不再产生里程碑（否则证据随回答长度
  // 线性增长，而重复的“又是第一个 token”没有任何信息量）。
  void on_token_delivered(const std::string& token) override;
  // 生成开始与结束不单独产生里程碑：文本定稿由会话侧的 generation_done 表达，后端自己
  // 报告“完成”是局部事实，两者的线性化点不同，都记会让同一个事实出现两个时间点。
  void on_generation_started() override;
  void on_generation_completed() override;
  // 生成失败会被记下原因，使“首 token 缺失”在汇总里有一个可读的解释，而不是一个空洞。
  void on_generation_failed(const std::string& message) override;

  // ---- 设备侧播放边界（IPlaybackBoundaryObserver）----
  // 首次写出音频汇记为 playback_start。重复调用被忽略。
  void on_first_frame_written() override;

  // 结束记录并给出运行结论。幂等：只有第一次调用会记下 run_end 与结论，后续调用被忽略，
  // 因此调用方不需要保证“只调一次”。调用后不再接受新的里程碑。
  void finish(const RunOutcome& outcome);

  bool finished();

  // 里程碑快照（按步数升序）。调用方拥有副本。
  std::vector<MilestoneRecord> milestones();

  // 四类产物。每个都经过对应值对象的编码校验；编码失败返回空串而不是半截 JSON——
  // 半截产物比没有产物更容易被误读成有效证据。
  std::string manifest_json();
  std::string events_jsonl();
  std::string metrics_jsonl();
  std::string summary_markdown();

  // 该里程碑是否出现过（按名称）。
  bool has_milestone(std::string_view name);
  // 首次出现的步数；未出现时 found 为假，输出参数不被修改。
  std::uint64_t step_of(std::string_view name, bool& found);
  // 首次出现的自 run_start 起的单调微秒；未出现时 found 为假，输出参数不被修改。
  std::int64_t mono_us_of(std::string_view name, bool& found);
  // 因果顺序上“播放已经开始，而生成尚未结束”。它由步数判定，因此与主机无关、可复现：
  // 时间差可能小于时钟分辨率，步序不会。
  bool overlap_proven();
  std::size_t milestone_count();
  // 取消是否被受理过。为假时取消阶段的缺失是“本运行没有取消”，而不是“阶段丢了”。
  bool cancellation_observed();
  // 生成后端是否报告过 token。为假时 first_token 的缺失属于路径不同（L0/L1 直答）。
  bool generation_tokens_observed();

  // 清单字段的只读取值，供上层汇总复用同一份事实而不必再解析 JSON。
  const std::string& run_id() const noexcept { return config_.run_id; }
  const std::string& config_hash() const noexcept { return config_.config_hash; }
  const std::string& input_hash() const noexcept { return config_.input_hash; }
  const std::string& command_hash() const noexcept { return config_.command_hash; }

 private:
  // 追加一条里程碑：分配步数、入表。调用方必须已经持有 mutex_。name 必须来自
  // milestone 命名空间（常量的地址），因此本函数不复制名字的存储责任。
  void append_locked(const char* name, std::uint64_t generation, const char* source,
                     std::int64_t mono_us);
  // 首次出现的下标；未出现返回 false。调用方必须已经持有 mutex_。
  bool first_index_locked(std::string_view name, std::size_t& index) const;
  // 未测量原因。已出现过时返回空串。调用方必须已经持有 mutex_。
  std::string unmeasured_reason_locked(const char* name) const;

  RunEvidenceConfig config_;
  const IMonotonicClock& clock_;
  // run_start 的绝对读数：所有 mono_us 都是它到事件时刻的差值。取绝对值而不是假设起点
  // 为 0，是为了不把“时钟读数从 0 开始”当成一条隐含前提。
  std::int64_t origin_us_ = 0;
  // 结束时刻，在 finish() 里确定。
  std::int64_t end_us_ = 0;
  RunOutcome outcome_{};

  mutable std::mutex mutex_;
  std::vector<MilestoneRecord> milestones_;
  std::uint64_t next_step_ = 0;
  bool finished_ = false;
  bool first_token_recorded_ = false;
  bool first_frame_recorded_ = false;
  // 生成后端报告的失败原因；为空表示没有失败。它只影响“为什么没有首 token”的解释，
  // 不改变任何里程碑或指标。
  std::string generation_failure_;
  // 清单与摘要里的日历时间。它们只用于关联“这次执行发生在什么时候”，不参与确定性比较。
  std::string start_time_;
  std::string end_time_;
};

}  // namespace nexweave::observability

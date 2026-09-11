// Session L0/L1 语音路径：把输入音频、识别结果、路由决策、合成 PCM 和播放
// 收尾编排成一个可审计的串行轮次。本文件只依赖能力契约、领域值和既有状态机
// 与交互夹具，不包含任何模型、设备、线程或传输实现；真实适配器由外层注入。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "../backend/fake_rag.hpp"
#include "../capability/backend.hpp"
#include "interaction_contract.hpp"
#include "session_state_machine.hpp"
#include "speech_segmentation.hpp"

namespace nexweave::runtime {

// 一次轮次的输入：常驻 stream 身份、调用方视角的输入代际和一段已采集的 PCM 样本。
// 样本由调用方拥有，只在本轮 run() 期间被读取；本结构不保留引用。空样本或
// 短尾样本按任务 15 的契约处理（空列表或补零尾帧），Session 不截断、不重采样。
// generation 只作为调用方的输入身份记录，事件过滤使用 Session 自己持有的代际水位，
// 避免调用方传入过期值把旧结果重新标成当前轮次。
struct SessionTurnInput {
  std::string stream_id;
  std::uint64_t generation = 0;
  std::string request_id;
  std::vector<std::int16_t> pcm_samples;
};

// 一次轮次的验收输出。字段分成三类，便于测试和运行证据分别核对：
//   - 终态：state、terminal_marker、error、cancelled、completed；
//   - 可审计证据：route、decision（含命中与原因）、text、pcm_frames；
//   - 生命周期标记：input_done / playback_started / playback_done。
// 成功时标记提交顺序固定为“文本定稿（kGenerationDone）→ 播放开始（kPlaybackStarted）
// → 合成结束（kSynthesisDone）→ 播放结束（kPlaybackDone）→ 唯一成功终态”；其中
// 播放开始严格早于合成结束，这正是“生成未结束就开始播放”的流式重叠证据。取消与
// 失败路径改为提交“受理 → 旧输出封锁 → 执行退出 → 播放清理 → 唯一取消终态”。
// pcm_frames 是已经交给播放组件、由其负责播放或丢弃的帧；被取消丢弃的待播帧仍会
// 出现在这里（它们确实交付过），而 played_count() 才是真正播完的数量。
// L0/L1 不调用 LLM，因此本结构没有生成过程字段；L2/L3 的 token 流由后续任务
// 在 Thinking 阶段补充，并复用同一组生命周期标记。
struct SessionTurnResult {
  // 本轮的请求标识：原样回显调用方传入的 request_id，使结果能归回到具体一次操作，
  // 而不是只靠调用顺序对应。不影响任何判定逻辑。
  std::string request_id;
  SessionStateMachine::State state = SessionStateMachine::State::kIdle;
  // 默认取取消终态：只有确认播放完成才会被改写为成功终态，避免默认值伪装成功。
  runtime::ActivityMarker terminal_marker = runtime::ActivityMarker::kTerminalCancelled;
  domain::Error error{};
  bool cancelled = false;
  bool completed = false;
  backend::RagRouteLevel route = backend::RagRouteLevel::kL3;
  backend::RagRouteDecision decision;
  std::string text;
  std::vector<domain::AudioFrame> pcm_frames;
  bool input_done = false;
  bool playback_started = false;
  bool playback_done = false;
  // 本轮是否被“用户新语音”打断。它与控制意图取消（用户喊停）和设备失败区分开：
  // 三者都以取消终态收敛，但只有它为真时 interrupt_notice 才携带新语音的归属。
  bool interrupted_by_speech = false;
  // 触发打断的新语音归属快照：stream_id 与 segment_id 说明是哪条输入流里的第几次
  // 说话，start_sequence 说明该次说话的第一帧（含段头前置缓冲）。仅在
  // interrupted_by_speech 为真时有效。打断只作废旧回答的输出，不取走新语音，
  // 因此这些音频仍然留在常驻输入里，可以被下一轮完整取走。
  SpeechStartNotice interrupt_notice;
};

// 具备确定性播放节奏的输出组件契约。Session 只负责“把合成帧交给播放组件并在
// 轮次结束时确认播放完成”，具体节奏与设备写入由实现拥有。
// 调用约定：start/stop 幂等；必须先 start 再 render，stop 之后不得再 render，
// stop 同时释放本轮，使同一组件能被常驻会话的下一轮重新 start()。
// 判定为“停止已受理”后实现不得再写出新帧；已经写出的帧属于已播放声音，任何
// stop 都不能撤回，只能丢弃尚未播完的部分。
// 完成判据由调用方根据 played_count() 与“已写入帧数”比较得出，本接口不再提供
// 独立的完成查询，避免出现“轮次已停止但完成查询语义不明”的歧义。
// 实现不得抛出异常，失败通过 error() 报告；播放组件拥有其设备句柄或内存缓冲，
// 析构必须先停止写入再释放资源。
class IAudioPlayback {
 public:
  virtual ~IAudioPlayback() = default;

  // 开始一轮播放：允许组件打开设备或重置内部游标。重复开始返回 kAlreadyCompleted。
  virtual domain::OperationResult start() = 0;

  // 交付一帧待播放音频。调用方保证帧满足统一音频契约；组件返回 kDeviceFailure
  // 时必须停止本轮，不得写出半帧或部分数据。
  virtual domain::OperationResult render(const domain::AudioFrame& frame) = 0;

  // 停止本轮：丢弃尚未写出的排队帧，保留已经写出的声音（不可撤回）。
  // 幂等、不抛异常；返回后不得再有新帧被写出。
  virtual domain::OperationResult stop() noexcept = 0;

  // 轮询播放进度：设备时间可能在两次交付之间前进，调用方据此刷新“是否播完”。
  // 实现不得阻塞、不得睡眠、不得写设备；默认实现允许设备自身推进进度而无须轮询。
  // Session 在合成结束后调用一次，用来区分“已合成完但还没播完”和“确实播完”。
  virtual void poll() {}

  // 本轮的播放结果。ok 表示本轮未发生设备/写入失败；未开始时也是 ok。
  virtual domain::OperationResult error() const = 0;

  // 已交给播放组件但尚未播完的帧数。Session 用它执行显式容量策略（达到上限时本轮
  // 明确失败而不是静默丢帧），并判断“已写入设备但还没播完”还剩多少。调用不改变
  // 播放状态。
  virtual std::size_t pending_count() const noexcept = 0;

  // 已经播完的帧数。Session 用它判断本轮所需输出是否真的播完：只有
  // played_count()==written 且至少播完一帧时才算完成；调用不改变播放状态。
  virtual std::size_t played_count() const noexcept = 0;

  // 已经成功写出的帧副本，按写出顺序排列。调用方拥有副本，可据此把“已播出的
  // 声音”和“已合成但被丢弃的声音”分开记录；调用不改变播放状态。
  virtual std::vector<domain::AudioFrame> played_frames() const = 0;

  // 借用 Session 的停止标志：为真时不得再写出任何新帧，判定发生在写设备之前，
  // 因此“受理停止”到“停止发声”之间至多已有一帧在途。该指针必须比本对象活得久；
  // 传 nullptr 表示轮次收尾，解除借用以免悬空引用。
  virtual void set_cancelled_flag(const std::atomic<bool>* flag) noexcept = 0;
};

// 播放逻辑时钟：把“现在该播第几帧”从真实时间解耦。测试用整数毫秒推进虚拟
// 时间，从而在毫秒级预算内复现“合成已结束但播放尚未结束”的状态；真实设备
// 适配器用 steady_clock 实现同一接口。now_ms 只读且不阻塞；advance 由拥有
// 播放节奏的一方串行调用，不得与 render 并发。
class IPlaybackClock {
 public:
  virtual ~IPlaybackClock() = default;
  virtual std::int64_t now_ms() const noexcept = 0;
  virtual void advance(std::int64_t delta_ms) = 0;
};

// 由调用方提供的时间基准：播放帧的到期时间与运行证据都取自它。
class ManualPlaybackClock final : public IPlaybackClock {
 public:
  explicit ManualPlaybackClock(std::int64_t start_ms = 0) noexcept;

  std::int64_t now_ms() const noexcept override;
  // 非正增量被忽略，保证逻辑时间单调不减；可重复调用且不分配。
  void advance(std::int64_t delta_ms) override;

 private:
  std::int64_t now_ms_ = 0;
};

// 确定性播放夹具：模拟“设备缓冲区 + 播放时钟”。交付的帧立即写入注入的音频汇
// （因此没有“因为没人推进时间所以写不进去”的死锁），同时按逻辑时钟判定哪些帧
// 已经播完：第 k 帧（0 起）在逻辑时间达到 (k+1)×frame_ms 后算作已播放。
// 这样“合成结束”与“播放结束”就能用同一个时钟精确区分，而不依赖真实时间、睡眠
// 或线程调度：时钟不前进时，写入的帧会一直停留在“已写入、尚未播完”的状态。
// 资源归属：renderer 由调用方创建并在本对象整个生命周期内存活（本对象只保存
// 借用引用，不负责开关设备）；本对象不创建线程、文件、socket 或设备句柄。
// stop() 丢弃尚未播完的缓冲但不撤回已写出帧，符合“取消不撤销已播放声音”的约定。
// 线程安全：全部方法由调用方串行调用（内部互斥量只保护字段一致性，不赋予并发
// 语义）；唯一允许跨线程读取的是 set_cancelled_flag 借用的原子标志。
// 除标准库分配失败外不抛异常。
class LogicalClockPlayback final : public IAudioPlayback {
 public:
  // frame_ms 是每个 20 ms 帧消耗的逻辑时间；非正值回退到固定帧时长，
  // 避免零时长播放让每帧瞬间“到期”，从而掩盖播放与合成的重叠关系。
  LogicalClockPlayback(capability::IAudioSink& renderer, const IPlaybackClock& clock,
                       std::int64_t frame_ms = 20) noexcept;

  // 开始新一轮并清空上一轮的缓冲与统计；已开始或已停止时返回 kAlreadyCompleted。
  // 成功后 played_count()==0、pending_count()==0；不改变 renderer 的开关状态。
  domain::OperationResult start() override;

  // 交付一帧：已开始、未停止、未受理停止且无过期错误时立即写入 renderer，并按
  // 逻辑时钟把它计入“已播出”或“尚未播完”。写入失败返回设备错误且不推进计数。
  domain::OperationResult render(const domain::AudioFrame& frame) override;

  // 停止本轮并丢弃尚未播完的缓冲；幂等、无分配。已写出帧保留在 played_frames()
  // 与 renderer 中，供“取消不删除已播放声音”的断言使用。
  domain::OperationResult stop() noexcept override;

  // 按当前逻辑时钟刷新“已播完”计数：不写设备、不阻塞，供调用方在时间前进后查询。
  void poll() override;

  domain::OperationResult error() const override;

  // 已播完的帧数与尚未播完的帧数快照；调用不改变播放状态。pending_count()==0
  // 即表示“已写入设备的音频都播完了”，Session 与测试都据此判断播放是否结束。
  std::size_t played_count() const noexcept override;
  std::size_t pending_count() const noexcept override;

  // 已写出帧的独立副本，按写出顺序排列。
  std::vector<domain::AudioFrame> played_frames() const override;

  // 借用 Session 的停止标志：为真时不再写出任何新帧。该指针必须比本对象活得久
  // （Session 在整轮 run() 期间保证其存活）；传 nullptr 等价于“未受理停止”，
  // 用于轮次收尾时解除借用，避免悬空引用。
  void set_cancelled_flag(const std::atomic<bool>* flag) noexcept override;

 private:
  // 按逻辑时钟刷新“已播完”计数：只读时钟，不访问设备。内部在持锁状态下调用。
  void refresh_played_locked();
  capability::IAudioSink& renderer_;
  const IPlaybackClock& clock_;
  const std::atomic<bool>* cancelled_ = nullptr;
  // 本轮已经成功写入设备的全部帧，按写出顺序。stop() 不清空它：已写入设备的声音
  // 不可撤回，仍可作为“取消不删除已播放内容”的证据。
  std::vector<domain::AudioFrame> written_;
  // 已写入设备的帧数，以及其中按逻辑时钟已经播完的帧数。两者之差就是待播帧数，
  // 用计数而不是缓冲容器表达，避免清空缓冲时出现“已播完帧数比缓冲还大”的错位。
  std::size_t written_count_ = 0;
  std::size_t played_ = 0;
  std::int64_t frame_ms_ = 20;
  mutable std::mutex mutex_;
  domain::Error error_{};
  bool started_ = false;
  bool stopped_ = false;
};

// 单进程 Session 编排配置。字段是显式策略，避免把队列容量写死在代码里；
// 真实适配器与部署 profile 在后续任务注入对应实现。
struct SessionRuntimeConfig {
  // 合成 PCM 播放队列上限（帧）。达到上限时本轮以 kBackendFailure 失败，而不是
  // 静默丢弃中间帧：丢弃会让回答出现无法察觉的空洞，宁可明确失败。
  // 取 0 表示不限制队列长度，仅用于受控测试；长回答会线性占用内存。
  // 这里的“队列”指已经交给播放组件、尚未播完的帧，因此它就是缓冲上限。
  std::size_t playback_queue_capacity = 256;
};

// 控制动作是否为停止类语义。调用方据此决定是否立即请求统一取消；判定只使用
// 路由给出的动作名，不解析用户原始文本，因此新增动作不会误触发停止。
bool is_stop_control_action(const std::string& control_action);

// L0/L1 串行 Session 编排。职责：注册输入回调、逐帧送 ASR、按路由级别选择直答、
// 合成 PCM 并交给播放组件，最后按“文本定稿/合成结束/播放结束”分别标记并产生唯一
// 终态。它不实现检索、模型推理、设备驱动和传输；这些能力全部经构造函数注入，
// 生命周期由调用方保证且必须长于本对象。
//
// 调用模型：一个 SessionRuntime 代表一个常驻会话，可连续执行多个轮次；常驻输入
// stream 只在第一轮建立，后续轮次复用同一 stream 并继续推进输入序号，因此“旧回答
// 清理”不会删除新语音的开头。调用方在会话结束时调用 finish_stream() 关闭输入流。
//
// 线程与并发约定：run()、finish_stream() 与 cancel() 由调用方串行调度，同一时刻
// 只允许一个轮次在途。cancel() 的唯一合法并发来源是播放或能力组件在帧边界处的
// 回调（例如停止指令或设备停止事件），它只做原子置位和状态机迁移，不等待后端；
// 其他线程不得在 run() 期间调用本对象，否则会破坏“回调与状态迁移同线程串行”的
// 前提。状态机内部有互斥量保护，但本类不承诺更宽的并发语义。
//
// 取消语义：取消依次建立旧输出封锁（状态机推进代际，旧代事件被拒绝）、通知执行
// 退出、丢弃播放组件的排队帧、产生唯一取消终态。取消不撤回已经交付的 PCM，也不
// 删除常驻输入流；停止类控制意图在本轮不合成任何音频，因此不会把“停止”变成知识
// 问答，也不会为新回答留下残留输出。
//
// 打断语义（可选）：挂接 IBargeInMonitor 后，会话在每个播放交付边界消费一次
// “用户是否开始说话”。出现新语音即走同一条取消路径打断当前回答，并把新语音的
// 归属写入轮次结果。线性化点固定在“一帧已经交付给播放组件之后”：打断因此一定
// 发生在某一帧的边界上，不会在两次交付之间凭空改变决定。会话只消费通知、不取走
// 音频，所以新语音的开头仍然留在常驻输入里等下一轮取走。未挂接监视器时行为与
// 既有串行路径完全一致。
//
// 资源与失败：Session 不拥有线程、文件、socket 或设备句柄，只借用注入的能力对象；
// 唯一的自有资源是每轮结束即析构的标量与帧容器。任一阶段返回错误时，本轮按
// “封锁旧输出 → 退出执行 → 清理播放 → Cancelling → Idle”收敛并保留结构化错误，
// 不把部分输出报告为成功。deadline 由外层调度负责，本类不等待、不睡眠。
class SessionRuntime final {
 public:
  // 借用全部能力对象：asr/router/tts/playback 必须比本对象活得久；retriever 必须
  // 比 router 活得久。router 的取消封锁由本类在每轮开始时显式 reset()，调用方
  // 无需在轮次之间手动解除。构造不创建外部资源，失败只可能来自标准库分配。
  SessionRuntime(capability::IAsr& asr, capability::IRag& retriever,
                 backend::FakeRagRouter& router, capability::ITts& tts,
                 IAudioPlayback& playback, SessionRuntimeConfig config = {});

  SessionRuntime(const SessionRuntime&) = delete;
  SessionRuntime& operator=(const SessionRuntime&) = delete;

  // 同步执行一个 L0/L1 轮次并返回验收结果。空输入、无语音文本、控制意图、播放
  // 失败和取消都返回结构化错误或取消终态，绝不抛出业务异常；成功仅当所需输出
  // 全部播放完成，且完成标记的顺序为“文本定稿 → 合成结束 → 播放结束 → 唯一终态”。
  SessionTurnResult run(const SessionTurnInput& input);

  // 关闭常驻输入流：必须在所有轮次结束后、销毁本对象前调用。关闭后不再接受新
  // 轮次（run 返回 kInvalidInput），重复调用返回失败但不破坏已有证据。
  domain::OperationResult finish_stream();

  // 请求取消当前轮次：置位停止标志并把状态机推进到 Cancelling（幂等）。
  // 返回后不再有新的帧被提交给播放；已在途的帧至多完成当前一次交付。
  // 无轮次在途时调用是空操作，不影响下一次 run() 的新代际。
  domain::OperationResult cancel() noexcept;

  // 挂接打断监视器：会话在每个播放交付边界消费一次“用户新语音起音”通知，
  // 出现即按统一取消语义打断当前回答。monitor 是借用指针，必须比本对象活得久；
  // 传 nullptr 关闭监视，这是默认行为，与既有串行路径一致。
  // 每轮开始时监视器的遗留通知会被清空：那些通知属于本轮前后才出现的说话，
  // 把它们算到新回答头上会让新回答刚开口就被自己打断。
  void set_barge_in_monitor(IBargeInMonitor* monitor) noexcept;

  // 状态机只读访问：供测试与运行证据核对阶段轨迹和当前代际。
  const SessionStateMachine& state_machine() const noexcept;
  // 活动标记快照，顺序即提交顺序。
  std::vector<ActivityMarker> trace() const;

 private:
  // 在一个播放交付边界消费打断监视器：出现新语音就置位停止标志并把状态机推进到
  // Cancelling，同时记录归属。它只做一次非阻塞查询、一次原子置位和一次状态迁移，
  // 不等待后端；未挂接监视器或本轮已经取消时是空操作。
  // 同步前提：按 IAsr/ITts 的既有约定，能力回调在发起调用的线程上同步执行，因此本函数
  // 与 run() 同线程，speech_interrupt_ 等成员不需要原子化；ITts 另行保证 synthesize
  // 返回前所有回调都已结束，回调不会活过本轮。若后续适配器改为在独立回调线程上投递
  // 事件，必须先为这些成员补上同步并重新声明回调寿命，不能直接复用本实现。
  void poll_barge_in();
  // 清空监视器里遗留的起音通知；每轮开始时调用，避免把本轮之前的旧语音算成打断。
  void drain_barge_in_notices();
  // 结束本轮的状态机与活动标记：无论成功、取消还是失败都收敛到 Idle，并追加
  // 恰好一个终态标记。调用前必须已经停止播放并解除停止标志借用。
  void finish_turn(SessionTurnResult& result);
  // 把状态机收敛到 Idle：在途阶段先进入 Cancelling 再提交 cancel_complete；
  // 已是 Idle 时是空操作。用于所有失败与取消路径，保证轨迹里一定出现统一的
  // 取消收敛阶段，而不是直接从中间阶段跳回 Idle。
  void force_idle();

  capability::IAsr& asr_;
  backend::FakeRagRouter& router_;
  capability::ITts& tts_;
  IAudioPlayback& playback_;
  SessionRuntimeConfig config_;

  SessionStateMachine state_machine_;
  InteractionContractFixture contract_;
  std::atomic<bool> cancel_requested_{false};

  // 可选的打断监视器（借用）。nullptr 表示关闭打断监视：回调里因此不产生任何
  // 额外分支，既有串行路径的行为与开销都不变。
  IBargeInMonitor* barge_in_ = nullptr;
  // 本轮是否由新语音触发取消，以及该语音的归属；每轮开始时清空。
  bool speech_interrupt_ = false;
  SpeechStartNotice speech_interrupt_notice_;

  // 常驻输入流状态：stream_id 跨轮次保持，只有 finish_stream() 关闭。输入序号由
  // 交互契约夹具自己维护（本路径的逐帧音频由 ASR 消费，不经过夹具）。
  bool stream_started_ = false;
  std::string active_stream_id_;
  std::string active_request_id_;

  // ASR 最终文本的暂存区：run() 开始时清空，回调期间写入，路由前读取。
  // 代际水位不在这里缓存——它统一由 state_machine_ 提供，避免两份水位脱节。
  bool queue_overflow_ = false;
  std::string asr_text_;
};

}  // namespace nexweave::runtime

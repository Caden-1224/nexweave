// NexWeave 单进程 Session 应用：把能力实现、会话编排和输入生产者组装成一个可运行的
// 程序。本文件只做组装与生命周期管理，不实现检索、模型推理、设备驱动或传输；这些能力
// 全部由调用方注入，生命周期必须长于本对象。
//
// 它存在的理由：会话编排层只接受“一段已经采集好的 PCM”，而一个真正跑得起来的会话还需要
// 三件外面的事——输入从哪来、每一轮什么时候开始、退出时谁来收尾。把这三件事收在一个对象里，
// 命令行入口就只剩参数解析与信号处理，测试也可以在没有 shell 的情况下验收整条链路。
//
// 输入模式与生产者的唯一性：同一输入流在同一时刻只能有一个生产者，这是输入契约的一部分。
// 三种模式因此互斥，且各自只有一个生产者：
//   - 文本：把配置里的固定文本作为一次识别结果。它不打开音频源、不建立输入流，只投递一帧
//     静音驱动“识别完成”这一事件，因此文本模式既没有音频设备，也不存在第二个生产者。
//     固定文本由本对象持有的文本注入器交付（见实现文件的 TextEventInjector）：注入器包在
//     注入的识别后端外面，只在“结束帧”这一点替换文本，其余调用原样转发。因此文本与音频走
//     同一条“识别 → 路由 → 合成 → 播放”链路，而不是被直接写进会话状态。
//   - 固定文件音频：应用自己解析一个 WAV 文件，把样本按统一音频契约分帧后作为**一轮**输入。
//     文件在开始任何轮次之前一次性读入并校验，因此“文件是坏的”不会变成一个已经开始又
//     中途失败的轮次。
//   - 模拟常驻输入：由注入的常驻音频输入拥有采集生命周期，应用只按轮次取走已经完成的
//     语音段。采集跨轮次保持：一个回答被打断或被取消都不会关闭它，也不会清掉新语音。
//
// 资源归属：本类不创建线程、文件、socket 或设备句柄。它借用注入的能力对象与常驻输入；
// 文件模式打开的文件在 run() 内读完并关闭，不留下句柄；每轮结束即析构的只有标量与帧容器。
// 常驻输入的采集由 run() 打开、由 run() 关闭，中途失败也走同一条收敛路径。
//
// 线程与并发：run() 由调用方线程串行调用，同一时刻只允许一次 run()。request_stop() 与
// cancel_turn() 是仅有的两个允许从其他线程或信号处理路径调用的入口：前者只做一次原子置位，
// 后者转发会话的取消入口（同样只置位并做一次状态迁移），两者都不等待、不分配。
// 退出路径因此有两处检查点：轮次边界，以及常驻输入取段时的逐帧泵入——所以“一条持续很久的
// 说话”不会让进程退不出去，停止请求最多在一个泵入预算内生效，而这个预算由配置显式声明，
// 不依赖设备自己醒过来。
//
// 确定性与失败语义：全部输入都来自显式配置的确定性夹具，因此同一配置重复运行得到相同的
// 轮次数、路由、文本与逐帧 PCM。失败不抛出业务异常：音频文件非法在开始任何轮次之前返回
// kInvalidInput；单个轮次失败只影响那一轮，应用记录首个失败原因后继续（常驻输入下后续
// 说话仍然可用），并在结果里分别保留每轮的终态。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../capability/backend.hpp"
#include "../capability/generation_probe.hpp"
#include "../domain/error.hpp"
#include "resident_audio_input.hpp"
#include "session_runtime.hpp"

namespace nexweave::runtime {

// 一次运行的输入模式。三种模式互斥：同一输入流在同一时刻只有一个生产者。
enum class SessionAppInputMode : std::uint8_t {
  // 固定文本：text 字段就是这一轮的识别结果，不打开音频源。
  kText,
  // 固定文件音频：wav_path 指向一个 16 kHz/单声道/16 位 PCM 的 WAV 文件。
  kWavFile,
  // 模拟常驻输入：由注入的 ResidentAudioInput 提供语音段，可连续多轮。
  kSimulatedResident,
};

// 输入模式的机器可读名称，用于日志、运行证据与命令行回显。未知值返回空串。
const char* to_string(SessionAppInputMode mode) noexcept;

// 应用配置。所有容量都是显式策略：默认值适用于单进程 Mock 演示，命令行可以覆盖，
// 真实部署 profile 在后续任务注入对应实现。
struct SessionAppConfig {
  SessionAppInputMode mode = SessionAppInputMode::kText;

  // 会话与输入流身份。stream_id 必须非空且在整个运行期间不变：它是常驻输入的唯一身份，
  // 也是“新语音属于哪条流”的归属来源；中途更换会让输入序号串到另一条流上。
  // 每轮的 request_id 由应用按 “<stream_id>-turn-<n>” 生成（n 从 1 开始），使结果能归回到
  // 具体一次操作，而不是只靠调用顺序对应。
  std::string stream_id = "session-app";
  // 文本模式的固定识别文本。只有 kText 模式读取它。
  std::string text;
  // 文件音频模式的 WAV 路径。只有 kWavFile 模式读取它；相对路径按进程当前目录解析。
  // v1 只接受 16 kHz、单声道、16 位 PCM 的 RIFF/WAVE 文件，不做重采样或位深转换。
  std::string wav_path;

  // 常驻输入的容量策略（分段与等待队列）。只有 kSimulatedResident 模式读取它。
  ResidentAudioInputConfig resident_config{};
  // 会话编排配置：播放队列容量与文本片段上限等。三种模式共用。
  SessionRuntimeConfig session_config{};

  // 本次运行最多执行的轮次数；0 表示不限，直到输入结束或被停止。
  // 它是退出路径的显式开关（便于演示与门禁验证“退出能唤醒输入”），不改变会话语义。
  std::size_t max_turns = 0;
  // 一次取段最多允许推进的帧数：它是生产者空转的上界。取值必须大于 0：没有上界的泵入
  // 会在“源还在产出但始终不成段”的输入上无限循环，因此拒绝 0 而不是把它解释成“不限”。
  std::size_t pump_budget_per_take = 64;
};

// 输入模式配置的静态校验：模式与必填字段必须自洽（文本模式要有非空文本、文件模式要有
// 路径），容量字段必须在合法范围内。校验只读配置，不做任何 I/O。
// 失败返回 kInvalidInput，调用方应在打开输入或执行轮次之前把它当成配置错误处理。
// 注意它只看配置本身：“模拟常驻模式必须注入音频输入”取决于构造参数而不是配置，因此由
// 建立生产者时检查；两者都在开始任何轮次之前失败。
domain::OperationResult validate_app_config(const SessionAppConfig& config);

// 一次运行的验收结果。分层记录：运行级（因何收敛、首个运行级失败、清理结果）、轮次级
// （每轮的完整终态与证据）、输入级（常驻输入的产出账目）。
// “这次运行算不算成功”由调用方按运行级错误与轮次结果判断，本结构不提供单一成功布尔：
// 取消与失败都不是运行级错误，它们各自出现在对应轮次里，因此“停止了一个被打断的回答”
// 不会被报告成程序故障。
struct SessionAppRunResult {
  // 运行级错误：配置非法、模式与输入对象不匹配，或无法为任何轮次取得输入（含读取输入
  // 途中失败）。单轮失败不写这里（它属于那一轮的 error），避免把“某一轮失败”放大成
  // “整个应用失败”。
  domain::Error error{};
  // 退出时关闭输入的结果。ok 表示采集已经停止、阻塞等待者已经醒来；非 ok 表示“退出后
  // 不再有后台采集”这一事实不成立，调用方必须按设备/资源故障处理。
  domain::Error cleanup_error{};
  bool input_ended = false;    // 输入自然结束（生产者耗尽）而收敛。
  bool stopped_early = false;  // 因停止请求或 max_turns 到限而提前收敛。
  std::vector<SessionTurnResult> turns;
  std::size_t turns_completed = 0;  // 以成功终态收敛的轮次数。
  std::size_t turns_failed = 0;     // 以失败或取消收敛的轮次数。

  // 常驻输入的产出账目快照；非模拟常驻模式恒为 0。它与 turns 一起构成“产出—消费—丢弃”
  // 的核对依据。
  std::uint64_t frames_pumped = 0;
  std::size_t segments_queued = 0;
  std::size_t segments_dropped = 0;
};

// 运行期观察者。应用在每个轮次收敛后与输入流关闭后各报告一次，顺序即发生顺序，因此
// “首帧播放早于文本定稿”“哪一轮被取消”都能从报告序列直接读出，而不需要猜。
// 约定：实现必须非阻塞、不抛异常、不回调进本对象；它只观察，不参与编排。默认实现全部为空，
// 因此不需要观察的调用方可以传 nullptr。
class SessionAppObserver {
 public:
  virtual ~SessionAppObserver() = default;
  // 一轮已经收敛（终态已确定、播放已经停止）后调用一次。
  virtual void on_turn(const SessionTurnResult& /*result*/) {}
  // 常驻输入流成功关闭后调用一次；本次运行没有建立过输入流时不会调用。
  virtual void on_stream_closed(const std::string& /*stream_id*/) {}
};

// 单进程 Session 应用。构造只保存借用引用与配置副本，不打开输入、不读文件、不创建线程；
// 全部资源都在 run() 内建立并在 run() 返回前释放。
//
// 重复运行：同一对象可以多次调用 run()，但**模拟常驻模式只支持一次**——常驻采集的开始是
// 一次性的（打开源、复位判定器、段编号继续递增），第二次运行会在建立输入时以
// kAlreadyCompleted 作为运行级错误失败，而不是重放同一次采集。文本与文件模式没有常驻
// 资源，可以重复运行并得到一致结果。需要重复常驻场景时调用方应重建常驻输入对象。
//
// 借用关系与生命周期：asr/retriever/router/tts/playback 必须比本对象活得久，retriever 还必须
// 比 router 活得久（路由器持有它的引用）；llm 可为空，为空时 L2/L3 轮次明确失败而不是伪造
// 回答；resident 只在模拟常驻模式下使用，其他模式传 nullptr 即可——传入非空值也不会被使用，
// 因为生产者由配置显式选择，而不是由“谁非空”推断。
// 生成进度观察者同样按借用处理：它在 run() 期间被挂到后端上，因此必须活到 run() 返回之后
// （通常是整个进程生命周期）。
class SessionApp final {
 public:
  SessionApp(const SessionAppConfig& config, capability::IAsr& asr, capability::IRag& retriever,
             backend::FakeRagRouter& router, capability::ITts& tts, IAudioPlayback& playback,
             ResidentAudioInput* resident = nullptr, capability::ILlm* llm = nullptr);

  SessionApp(const SessionApp&) = delete;
  SessionApp& operator=(const SessionApp&) = delete;

  // 执行一次完整运行：按配置建立输入、逐轮驱动会话、在输入结束或停止请求后收尾。
  // 返回后不会再有轮次在途，常驻输入已关闭，播放已停止；不抛出业务异常，全部失败以错误码
  // 报告。重复运行的适用范围见类注释：文本与文件模式可以重复运行，模拟常驻模式不行。
  SessionAppRunResult run(SessionAppObserver* observer = nullptr);

  // 请求停止本次运行：立即返回，不等待当前轮次。语义是“输入与执行该退出了”，与“取消一次
  // 回答”区分开——它只影响运行循环的收敛，不改变任何轮次的终态判定。
  // 本入口是唯一允许从其他线程或信号处理路径调用的方法：只做一次原子置位，不分配、
  // 不阻塞、不抛异常。停止请求在下次运行开始时会自动清除，因此重复运行不需要手工复位。
  void request_stop() noexcept;

  // 请求取消当前轮次（转发会话的统一取消入口）。
  //
  // 合法来源只有两类：轮次之间的调用方决策，以及播放/能力组件在投递回调里的停止指令
  // （例如设备侧“用户按了停止”）。后者是能力契约明确允许的重入位置，因此本方法在回调里
  // 也只触碰 ILlm/ITts 的取消入口——按 capability/backend.hpp 的约定，IAsr 不允许在 feed
  // 回调中被重入，所以这里不会去取消识别。取消没有在途轮次时是幂等空操作。
  void cancel_turn() noexcept;

  // 本次运行是否已经收到停止请求（供收尾与证据核对）。
  bool stop_requested() const noexcept;

  // 活动标记快照，顺序即提交顺序。它把“播放开始早于文本定稿”“取消四段事实齐全”等
  // 跨阶段的先后关系暴露给运行证据，而不需要调用方自己重建一遍。
  std::vector<ActivityMarker> trace() const;

  // 挂接生成进度观察者（借用指针，可为 nullptr）。本对象把它转交给会话层：后端支持进度
  // 观测时，会话在每次生成期间把后端报告的事实同步转发给它，使“首帧播放早于生成结束”
  // 这类跨阶段事实可以被外部按事件顺序核对。未挂接时不产生任何额外分支；后端不支持进度
  // 观测时挂接被忽略，生成与取消语义不变。观察者必须活到本对象析构。
  void set_generation_observer(capability::IGenerationObserver* observer) noexcept;

 private:
  // 输入生产者接缝：每次调用交出“下一轮要处理的音频”，耗尽后结束运行。
  // 三种模式各有一个实现，同一时刻只有一个实例存在，因此“唯一生产者”是构造性质而不是
  // 运行时约定。实现不得创建线程或后台任务，也不得在 next() 里阻塞到无法按轮询周期返回。
  //
  // 它必须是公开类型：具体实现定义在实现文件的匿名命名空间里，通过基类指针被本类持有；
  // 隐藏它只会让实现无法继承，而不会带来任何封装收益（外部拿不到本类的 producer_ 成员）。
 public:
  class TurnProducer {
   public:
    virtual ~TurnProducer() = default;
    // 建立输入（打开音频源等）。只调用一次；失败时调用方必须放弃本次运行并关闭已建立的
    // 部分。默认实现表示“无需建立”，因此纯内存生产者不必重写它。
    virtual domain::OperationResult start() { return domain::OperationResult::success(); }
    // 取得下一轮输入。成功时写入 output 并返回成功；不会再有任何输入时返回
    // kAlreadyCompleted（这是正常结束，不是故障）；确实失败时返回该错误。两种结束原因
    // 必须分开报告，否则“输入读坏了”会被当成“说完了”而静默收尾。
    virtual domain::OperationResult next(SessionTurnInput& output) = 0;
    // 输入是否已经自然结束（不会再产出）。用于区分“输入结束”与“提前停止”。
    virtual bool input_ended() const noexcept = 0;
    // 本次运行是否建立过输入流（供观察者决定是否报告“输入流已关闭”）。
    virtual bool stream_established() const noexcept = 0;
    // 上一次 next() 是否因为停止请求而放弃（而不是因为输入已经读完）。两个原因必须分开
    // 报告：把它们合并会让“被打断的运行”在证据里显示成“正常收尾”。默认实现表示
    // “从不因停止而放弃”，纯内存生产者因此不必重写它。
    virtual bool stopped_by_request() const noexcept { return false; }
    // 本次运行要交付的固定识别文本。只有文本模式返回非空值，其他模式返回空串，表示
    // “识别结果来自音频”。之所以由生产者而不是配置直接提供：文本模式与音频模式的差别
    // 就在“这一轮的识别文本从哪来”，把它放进生产者，会话层只需面对一种输入形态。
    // 返回副本而不是引用：文本的所有权在生产者，副本让调用方不必关心它的生命周期。
    virtual std::string scripted_text() const { return std::string(); }
    // 收尾：关闭由生产者拥有的输入资源。必须幂等，不抛出；失败以错误码返回。
    virtual domain::OperationResult close() noexcept = 0;
  };

 private:
  // 按配置构造唯一的生产者。配置非法、或模式要求的输入对象缺失时返回 nullptr 并填入错误，
  // 此时不会建立任何输入。
  std::unique_ptr<TurnProducer> make_producer(const SessionAppConfig& config, domain::Error& error);
  // 取下一个轮次输入：轮次之间的停止请求与轮次上限在这里被受理。
  // 返回 false 表示本次运行应当在当前轮次之后收敛；ended 说明是输入耗尽还是被停止。
  bool take_next_input(SessionTurnInput& output, bool& ended, domain::Error& error);
  // 采集常驻输入的产出账目快照；非常驻模式保持为 0。
  void account_resident_input(SessionAppRunResult& result) const;

  SessionAppConfig config_;
  capability::IAsr& asr_;
  capability::IRag& retriever_;
  backend::FakeRagRouter& router_;
  capability::ITts& tts_;
  IAudioPlayback& playback_;
  ResidentAudioInput* resident_ = nullptr;
  capability::ILlm* llm_ = nullptr;
  capability::IGenerationObserver* generation_observer_ = nullptr;
  // 会话对象：构造时建立，识别后端固定为本对象持有的文本注入器（它再转发给注入的识别
  // 对象）。固定文本只在每次运行时写进注入器，因此会话不需要「按模式换后端」这种会造成
  // 悬空引用的设计。
  std::unique_ptr<SessionRuntime> session_;
  // 文本注入器：生命周期与本对象相同，因此会话持有的引用不会悬空。每次运行结束都会清空
  // 其中的文本并解除已注册的回调，避免上一次运行的文本或回调留在里面。
  std::unique_ptr<capability::IAsr> injector_;
  // 本次运行唯一的生产者；每次 run() 重建，run() 返回前销毁，因此生产者的生命周期
  // 严格短于本次运行，不会跨运行复用过期状态。
  std::unique_ptr<TurnProducer> producer_;
  // 本次运行已经开始的轮次数（含失败与取消的轮次），用于 max_turns 判定。
  std::size_t result_turns_ = 0;
  // 停止请求：唯一允许跨线程写入的状态。
  std::atomic<bool> stop_requested_{false};
};

}  // namespace nexweave::runtime

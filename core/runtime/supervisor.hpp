// NexWeave 进程内 Supervisor（会话监督器）：管理一台设备上**唯一**活跃会话的创建、
// 状态查询、取消与退出，并明确“忙碌”和“清理中”的行为。
//
// 它解决什么问题
// --------------
// 会话编排层（SessionRuntime/SessionApp）只回答“这一轮怎么办”；把会话跑起来还需要有人回答
// “现在有没有会话、能不能再开一个、用户喊停之后到底停干净没有”。本类把这三个问题收在一处，
// 并让答案可以被外部轮询，而不是靠调用方自己拼装标志位。
//
// 单活跃与忙碌
// ------------
// 同一时刻只允许一个会话占用槽位，这是设备级约束（同一套音频设备不允许两路回答交错）。
// 槽位被占用时再次创建**不会**排队、不会替换、也不会并发跑第二个会话，而是立即返回结构化
// 忙碌错误（domain::ErrorCode::kBusy），且不留任何副作用：调用方据此回一个“忙，稍后再来”，
// 而不是把设备拖进两路并发。
//
// 语音打断**不通过**并发创建实现：打断发生在会话内部，由会话按生成代际作废旧回答的输出
// （见 SessionRuntime 的打断语义）。本类因此刻意不提供任何“替换当前会话”“抢占槽位”的入口，
// 否则“单活跃”就只是调用方自律，而不是可验收的约束。
//
// 三个必须能区分的外部状态
// ------------------------
//   - 工作（kStarting/kActive）：会话已受理，拥有者正在建立或执行。
//   - 取消清理中（kQuiescing）：停止已经受理，但拥有者还没有交还资源。
//   - 不可用（kUnavailable）：清理失败或超出等待上限，槽位**不再复用**。
// 三者由 status() 的 SupervisorState 直接区分；kClosed 表示监督器已经退出，与不可用不同：
// 前者是正常的终态，后者是故障留下的死槽位。
//
// 受理与清理完成是两件事
// ----------------------
// cancel() 先同步受理（置位 + 状态推进 + 通知拥有者），再按预算等待清理；返回值把
// “是否受理”和“清理是否在预算内完成”分开报告。预算内没等到清理返回 kTimeout，并且
// **不**把槽位标记为可复用——它仍然停在 kQuiescing，下次调用可以继续等。这条规则的意义是：
// 取消响应永远不能被当成“设备已经静默”的证据，只有清理完成（或明确的失败终态）才是。
//
// 清理失败的收敛
// --------------
// 只有拥有者的清理成功返回，槽位才回到 kIdle。清理失败、或拥有者始终清理不完，
// 槽位进入 kUnavailable 并永久保留该事实（last_cleanup_error），此后创建一律失败而不是
// “再试一次也许就好了”：在一个可能还握着设备句柄的槽位上重新开会话，比明确拒绝更危险。
//
// 线程归属
// --------
// 每次会话由本类创建**恰好一个**工作线程，并在该线程上串行调用拥有者的 start()/run()/cleanup()；
// 拥有者因此不需要为这三个入口加锁。控制入口（start/cancel/shutdown/status/wait_for_slot）由
// 调用方线程调用，与工作线程之间只用一把互斥量和条件变量交接状态；它们都不会等待整段推理。
// 唯一允许在工作线程之外触碰拥有者的是 request_stop()，能力契约要求它非阻塞、不分配、不抛异常。
//
// 资源归属
// --------
// 本类创建：一个 std::thread、一个堆上的槽位状态、以及由工厂产出的拥有者对象。线程由本类
// 创建与 join（无法在预算内收敛时 detach，见析构注释）；拥有者对象在 start() 的调用线程上
// 由工厂创建，之后由工作线程独占使用，并在清理返回后由工作线程释放——它是唯一使用过该对象的
// 线程。工作线程只持有槽位的 shared_ptr，不持有本对象指针，因此即使本对象先析构，线程也不会
// 访问已释放内存。
//
// 确定性
// ------
// 本类不读时钟做决策（只在有界等待里用 steady_clock 计时），不睡眠，不使用任何全局状态；
// 同一组操作序列在同样的拥有者行为下产生同样的状态、错误码与计数。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "../domain/error.hpp"

namespace nexweave::runtime {

// 监督器对外可见的生命周期状态。数值不参与线协议，可自由重排；字符串名用于日志与证据。
enum class SupervisorState : std::uint8_t {
  // 空闲：没有会话占用槽位，可以受理新的创建。
  kIdle = 0,
  // 建立中：创建已受理，拥有者的 start() 尚未返回。
  kStarting = 1,
  // 工作中：拥有者的 run() 正在执行。会话主体尚未结束。
  kActive = 2,
  // 清理中：拥有者正在交还资源。进入这个状态有两条路径——停止被受理，或会话主体已经自然
  // 结束——两者都意味着槽位不可复用；用 SupervisorStatus::cancel_accepted 区分是哪一条。
  kQuiescing = 3,
  // 不可用：清理失败或超出等待上限。槽位永久不可复用，只能退出。
  kUnavailable = 4,
  // 已退出：监督器已终结，不再受理任何操作。
  kClosed = 5
};

// 状态名的机器可读形式，用于日志、运行证据与测试断言。未知值返回空串（与领域内其他
// to_string 重载一致），不抛异常、不分配。
const char* to_string(SupervisorState state) noexcept;

// 一次会话的启动参数。work_id、session_id、request_id 都是**身份**，本类只负责携带与回显，
// 不解析其结构；session_sequence 由本类填写（调用方传入的值会被覆盖），它是槽位的会话序号。
struct SupervisorSessionSpec {
  // 任务身份。空串表示由监督器生成 "work-<session_sequence>"：会话序号单调递增且不回收复用，
  // 因此同一个 work_id 永远不会指代两次不同的会话，旧身份不会与新身份撞车（ABA）。
  std::string work_id;
  // 会话身份。调用方自定；为空时保持为空，不参与任何判定。
  std::string session_id;
  // 发起本次创建的控制请求身份。原样回显到状态快照，便于把会话归回到具体一次操作。
  std::string request_id;
  // 本类分配并回填给拥有者的会话序号（从 1 开始，每次成功受理创建 +1，永不回绕复用）。
  // 调用方传入的值会被覆盖；它是只读输出字段，保留在同一个结构里是为了让拥有者不必
  // 再拿一个并行参数。
  // 与会话内部的“生成代际”不是同一个东西：序号标识**哪一次会话**，由监督器在每次创建时
  // 分配；生成代际标识同一会话内哪一代回答，由会话层在取消或重开时推进。
  std::uint64_t session_sequence = 0;
};

// 一次生命周期控制操作（cancel/shutdown）的结果。
//
// 字段刻意分开而不是压成一个布尔，因为规格要求把这些事实分别观测：
//   - accepted：cancel 时表示本次调用确实受理了停止（会话此前已受理也算“已受理”，只是本次
//     没有再改变状态）；shutdown 时表示本次调用确实把监督器推进到了已退出。空闲槽位上的
//     cancel 是幂等空操作，accepted 为假。
//   - cleanup_completed：清理是否已经在预算内结束（成功或明确失败都算结束）。它不表示清理
//     是成功的——那要看 error。
//   - state：受理之后槽位的状态快照，调用方不必再查一次。
//   - error：ok 表示操作按预期完成；kTimeout 表示受理成功但清理未在预算内完成；清理本身
//     失败时原样带出该失败；其余错误码表示操作根本没有被受理（例如监督器已退出）。
struct SupervisorControlOutcome {
  bool accepted = false;
  bool cleanup_completed = false;
  SupervisorState state = SupervisorState::kIdle;
  domain::Error error{};
  bool ok() const noexcept {
    return error.ok();
  }
};

// 槽位状态快照。全部字段都是调用瞬间的一致快照（同一把锁下读出），调用方拥有副本。
struct SupervisorStatus {
  SupervisorState state = SupervisorState::kIdle;
  // 当前（或最近一次）会话的序号；从未受理过创建时为 0。
  std::uint64_t session_sequence = 0;
  // 当前（或最近一次）会话的身份；没有会话时为空串。会话收敛后仍保留最后一次的身份，
  // 使“刚刚结束的是哪一次”在证据里可查。
  std::string work_id;
  std::string session_id;
  std::string request_id;
  // 当前在途会话是否已经受理过停止。它与 state==kQuiescing 一起把两种清理区分开：
  // 两者同时为真表示“取消清理中”，只有 state 为 kQuiescing 表示“会话已自然结束、正在收尾”。
  // 会话收敛后复位为假（那次取消由 sessions_cancelled 记账）。
  bool cancel_accepted = false;

  // 账目：用于核对“忙碌拒绝没有偷偷创建会话”“清理失败没有被计成完成”。
  // 不变量：每一次被受理的创建恰好让 completed / cancelled / failed 三者之一加一，因此
  // 三者之和等于**被受理的创建次数**——它包含建立失败与建立超时，所以不等于 sessions_started。
  // 被忙碌拒绝的创建不计入这三项，只计入 busy_rejections。
  // 计数在会话真正收敛时才落账：建立超时后卡住不返回的会话在它收敛前不会出现在账目里。
  std::uint64_t sessions_started = 0;    // 拥有者 start() 成功、真正进入过工作态的会话数
  std::uint64_t sessions_completed = 0;  // 未受理停止、run() 成功、cleanup() 也成功
  std::uint64_t sessions_cancelled = 0;  // 收敛前已经受理过停止，且 cleanup() 成功
  std::uint64_t sessions_failed = 0;     // 其余：建立失败、建立超时、执行失败或清理失败
  std::uint64_t busy_rejections = 0;     // 因槽位被占用而被拒绝的创建次数

  // 最近一次会话的三段结果。它们各自独立：建立失败时 run/cleanup 仍会按顺序走到，
  // 因此“根因在建立”和“清理也失败了”可以同时被看到。
  domain::Error last_start_error{};
  domain::Error last_run_error{};
  domain::Error last_cleanup_error{};

  // 槽位是否可以受理新的创建。只有 kIdle 为真：kUnavailable 与 kClosed 都不可复用。
  bool slot_reusable() const noexcept {
    return state == SupervisorState::kIdle;
  }
};

// 会话拥有者：一次会话的全部资源都由它持有，三个生命周期入口都由监督器在**同一个工作线程**
// 上串行调用，因此实现内部不需要为它们加锁。
//
// 调用顺序由监督器保证，实现不得假设其他顺序：
//   start() → （仅当 start() 成功）run() → cleanup() → 对象析构
// 即：start() 失败也一定会走到 cleanup()，因为失败前可能已经建立了部分资源。
class ISessionOwner {
 public:
  virtual ~ISessionOwner() = default;

  // 建立会话：打开输入、准备资源。在工作线程上调用一次，返回后监督器才认为会话进入工作态。
  // 实现必须尽快返回：本入口是“启动就绪”的信号，不能在这里跑推理或无限期等待设备。
  // 失败时返回结构化错误，监督器会跳过 run() 但仍然调用 cleanup() 回滚。
  virtual domain::OperationResult start() = 0;

  // 执行会话主体，直到输入耗尽、被停止或被取消；必须在收敛后返回，不得挂起等待外部事件。
  // 取消不是失败：被取消时实现应返回被取消的结果，而不是伪装成成功或上报后端故障。
  virtual domain::OperationResult run() = 0;

  // 交还 start()/run() 建立的全部资源（设备句柄、文件、子进程、内部线程等）。
  // 在工作线程上、run() 之后调用一次；必须幂等、不抛异常。
  // **只有它返回成功，监督器才允许把槽位重新标记为可用**；返回失败意味着资源状态不明，
  // 槽位转为不可用。实现不得在这里等待推理：要等待就在自己的预算内等，并把超时以
  // kTimeout 返回，让监督器据此判定槽位不可复用。
  virtual domain::OperationResult cleanup() noexcept = 0;

  // 请求停止当前会话。可从任意线程调用（包括信号处理路径），必须非阻塞、不分配、不抛异常、
  // 可重复调用。它只是“请求”：真正的收敛由 run() 返回后由监督器确认。监督器允许在持有
  // 内部互斥量的状态下调用它，正是靠这条契约消除“取到过期拥有者指针”的窗口。
  virtual void request_stop() noexcept = 0;
};

// 拥有者工厂：为每一次会话产出**全新**的拥有者实例。
//
// 之所以要工厂而不是复用一个拥有者对象：会话收敛后槽位会被下一个会话复用，如果两次会话
// 共用同一个拥有者，上一次的身份、缓冲和错误状态就会漏进下一次。每次新实例让“重建身份”
// 成为构造性质，而不是清理是否彻底的问题。
class ISessionOwnerFactory {
 public:
  virtual ~ISessionOwnerFactory() = default;

  // 创建一次会话的拥有者。在工作线程创建之前、由 start() 的调用线程同步调用，因此它可以
  // 阻塞一小段时间（打开设备、分配缓冲），并且失败可以被同步报告。
  // 返回 nullptr 表示本次创建失败，必须往 error 里填入结构化错误码与诊断文本。
  // 返回非空表示成功，error 被忽略。实现不得保存 spec 的引用：spec 是本次调用的临时值。
  virtual std::shared_ptr<ISessionOwner> create(const SupervisorSessionSpec& spec,
                                                domain::Error& error) = 0;
};

// 监督器配置。两个预算都必须为正：把等待上限定成 0 会让“有界等待”退化成“从不等待”，
// 从而无法区分“清理很快”和“根本没等”。构造不抛业务异常，非法值会整份回退到默认配置；
// 希望把配置错误当错误处理的调用方应先调用 validate_supervisor_config()。
struct SupervisorConfig {
  // 等待拥有者 start() 返回（启动就绪）的上限。超时后槽位转入不可用：一个迟迟不返回的
  // 建立过程说明外部资源状态未知，继续把它当可用槽位会让后续会话踩在未知状态上。
  std::chrono::milliseconds start_wait_budget{5000};
  // 等待拥有者 cleanup() 返回（清理完成）的上限。它是 cancel()/shutdown() 的默认预算，
  // 也是析构时的等待上限。
  std::chrono::milliseconds cleanup_wait_budget{5000};
};

// 配置校验：只读配置，不创建线程、不访问外部资源。预算非正返回 kInvalidInput；
// 成功不改变输入。调用方应在构造监督器之前把它当成配置错误处理。
domain::OperationResult validate_supervisor_config(const SupervisorConfig& config);

// 进程内会话监督器。构造只校验配置并保存借用引用，不创建线程、不访问设备；
// 全部线程与槽位资源在 start() 建立，在会话收敛或析构时释放。
//
// 借用关系：factory 必须比本对象活得久（只在 start() 的调用线程上被使用）。
//
// 线程安全（这是调用方必须遵守的契约，不只是实现细节）：
//   - status()、wait_for_slot()、cancel()、shutdown() 可以任意并发调用，也可以与一次在途
//     会话并发调用：它们只用一把互斥量与条件变量交换状态，不等待整段推理。
//   - start() 是**独占**入口：同一时刻只允许一次 start()。这一条由槽位状态保证而不是由调用
//     方自律——第一个调用在锁内把槽位推进到 kStarting，并发的第二个只能在锁外看到忙碌。
//   - 析构同样是独占的：调用方必须保证析构期间没有任何线程还在调用本对象的其他方法
//     （正在收尾的工作线程不算，它只持有槽位、不访问本对象）。
//   - 工作线程句柄由 start() 与析构独占读写，其他入口都不碰它；因此并发调用 cancel() 与
//     shutdown() 不会产生对同一个线程句柄的竞争。
//
// 重复创建与退出：
//   - 槽位空闲时再次创建会建立一个全新的会话（新序号、新拥有者），这是正常用法；
//   - 槽位被占用（kStarting/kActive/kQuiescing）时创建返回 kBusy，不排队、不替换；
//   - 槽位不可用（kUnavailable）时创建返回 kBackendFailure，直到退出为止都不再受理；
//   - 监督器已退出（kClosed）时创建返回 kAlreadyCompleted；
//   - shutdown() 幂等：已经退出时再次调用成功返回且不改变任何状态。
class Supervisor final {
 public:
  explicit Supervisor(ISessionOwnerFactory& factory, SupervisorConfig config = {});

  // 析构：受理停止并在配置的清理预算内等待会话收敛，然后回收工作线程。
  // 预算内没有收敛时**detach**线程并放弃等待：本对象此后不再被工作线程访问（线程只持有
  // 槽位的 shared_ptr），因此不会悬空；代价是那个槽位与线程会一直存在到清理自己结束。
  // 之所以不无限等待：析构可能在信号处理或错误路径上被调用，把它变成不可中断的阻塞点
  // 会让“退出”本身失去上界。之所以不 std::terminate：那会把一次清理超时升级成进程崩溃。
  ~Supervisor();

  Supervisor(const Supervisor&) = delete;
  Supervisor& operator=(const Supervisor&) = delete;

  // 受理一次创建并等待启动就绪。返回 ok 表示会话已经进入工作态（kActive），此后由工作线程
  // 执行到收敛，不需要调用方继续驱动。
  // 失败语义：
  //   kBusy            槽位已被占用（含建立中与清理中）；无副作用，可稍后重试。
  //   kAlreadyCompleted 监督器已退出；不可重试。
  //   kBackendFailure  槽位不可用；或工厂拒绝创建（此时用工厂给出的错误码）。
  //   kTimeout         建立超出 start_wait_budget；槽位转入不可用。
  //   其他             拥有者 start() 的原始错误码；槽位在回滚清理成功后回到 kIdle。
  // 本方法阻塞的是“建立”，不含推理；返回后调用方线程不再被会话占用。
  domain::OperationResult start(const SupervisorSessionSpec& spec);

  // 请求取消当前会话并可选地等待清理。
  // expected_session_sequence 为 0 表示不校验目标；非 0 时必须等于当前会话序号，否则返回 kInvalidInput
  // 且不产生任何副作用——这条校验让“针对上一次会话的迟到取消”不会打到新会话上。
  // wait_budget 为空表示用配置里的 cleanup_wait_budget；给 0 表示只受理、不等待——此时
  // 返回值是调用瞬间的快照，cleanup_completed 说明清理是否碰巧已经结束，而**不会**产生
  // 超时错误：没有等待就没有“预算用尽”这回事。
  // 返回：accepted 表示本次停止确实被受理（会话正在工作或建立中）；会话已经进入自然收尾、
  // 或本来就空闲时 accepted 为假但操作成功——这两种情况下旧结果都会照常作废，只是没有
  // “被打断的工作”可供记账。cleanup_completed 表示槽位是否已在预算内收敛；kTimeout 表示
  // 受理成功但预算内没有收敛，槽位仍停在 kQuiescing 且不可复用，调用方可以稍后带同样的
  // 预算再等一次。清理本身失败时终态不变，但失败原因出现在 error 里：取消响应不能因此被
  // 当成“设备已经静默”的证据。
  SupervisorControlOutcome cancel(
      std::uint64_t expected_session_sequence = 0,
      std::optional<std::chrono::milliseconds> wait_budget = std::nullopt);

  // 退出监督器：受理停止、等待清理、把状态推进到 kClosed。
  // 幂等：已经退出时成功返回（accepted=false，状态不变）；不可用的槽位同样可以退出。
  // 若预算内没有收敛，状态保持 kQuiescing 且返回 kTimeout，重复调用会继续等待，因此“退出”
  // 本身不会因为一次超时而变成不可完成的动作。
  // accepted 在本次调用确实推进了退出（从在途、空闲或不可用进入 kClosed）时为真。
  // 退出成功不代表上一次清理是干净的：清理失败会作为 error 与 state==kClosed 同时出现，
  // 调用方用 state 判断“退没退”、用 error 判断“上一次清理干不干净”，两者不会互相掩盖。
  SupervisorControlOutcome shutdown(
      std::optional<std::chrono::milliseconds> wait_budget = std::nullopt);

  // 状态查询：返回一致快照，不阻塞（只争用一把互斥量），不触发任何状态迁移。
  SupervisorStatus status() const;

  // 等待槽位释放（回到 kIdle/kUnavailable/kClosed）。只观察，不触发取消：调用方用它把
  // “会话自然收敛”变成可等待的事实，而不必轮询。预算内没有释放返回 false。
  // budget <= 0 表示只做一次快照判断。
  bool wait_for_slot(std::chrono::milliseconds budget) const;

 private:
  // 会话槽位：监督器与工作线程共享的可变状态。定义在实现文件里，外部拿不到它的布局，
  // 因此后续给槽位加字段不需要改公共头文件。
  struct Slot;

  // 工作线程主体。刻意做成静态成员：它只捕获槽位与拥有者的 shared_ptr，不捕获 this，
  // 于是“析构时 detach”不会留下指向已销毁监督器的访问路径。
  static void run_session(const std::shared_ptr<Slot>& slot,
                          const std::shared_ptr<ISessionOwner>& owner);

  // 受理停止（幂等）。前置：调用方已持有 slot_->mutex_。它只做置位、状态迁移和一次
  // request_stop()，不等待、不分配。
  // 返回 true 表示本次调用确实把在途会话推进到了清理态（kStarting/kActive → kQuiescing）。
  // 返回 false 表示没有“正在工作的会话”可打断：要么已经因为本方法进入清理（停止已经受理，
  // 不重复通知拥有者，避免给后端增加与取消无关的副作用面），要么会话主体已经自然结束、
  // 只是资源还没交还——那时取消不会改变任何结果，如实报告“没受理”比假装受理更准确。
  bool accept_stop_locked();

  // 在有界预算内等待槽位收敛，并给出控制结果快照。不持锁调用；会阻塞该线程最多 budget。
  SupervisorControlOutcome await_settled(std::chrono::milliseconds budget);

  // 取有效预算：空值用配置默认；有值时必须非负，负值按 0 处理（等价于只做一次快照），
  // 因为“负的等待时长”没有有意义的行为，而把它当成“无限等待”会让一次笔误变成挂起。
  std::chrono::milliseconds effective_wait(
      const std::optional<std::chrono::milliseconds>& budget) const;

  ISessionOwnerFactory& factory_;
  SupervisorConfig config_;
  // 槽位状态。shared_ptr 让工作线程与监督器共享同一份状态，而 detach 后仍然安全。
  std::shared_ptr<Slot> slot_;
  // 工作线程句柄。由监督器独占持有：只在确定线程已经结束（槽位收敛）或析构放弃等待时操作。
  std::thread worker_;
};

}  // namespace nexweave::runtime

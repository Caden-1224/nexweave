// 进程内 Supervisor 生命周期的集成夹具。
//
// 职责：在监督器这一层验收“一台设备只允许一个活跃会话”这件事到底是怎么成立的：创建与
// 退出的幂等、忙碌拒绝的结构化错误、工作/取消清理/不可用三种状态的可区分、清理未完成时
// 槽位不得复用、有界等待与其超时终态、以及迟到的取消不会打到新会话上。
//
// 保护的不变量（每个用例在断言旁注明它保护哪一条）：
//   1. 单活跃：槽位被占用时创建不排队、不替换、不并发跑第二个会话，且不产生第二个拥有者。
//   2. 忙碌结构化：占用期间的新请求得到 domain::ErrorCode::kBusy，而不是一个含糊的失败。
//   3. 状态可区分：工作（kActive）、取消清理（kQuiescing + cancel_accepted）、不可用
//      （kUnavailable）三种情形在 status() 上互不混淆。
//   4. 清理未完成不得复用：清理失败或等待超时都让槽位不可复用；只有拥有者成功交还资源、
//      且这次会话没有被退休，槽位才回到 kIdle。
//   5. 受理与清理完成分离：cancel() 的 accepted 与 cleanup_completed 是两个独立事实，
//      超时只说明“还在清理”，不说明“取消没生效”，更不说明“设备已经静默”。
//   6. 会话序号不回收：每次受理的创建推进一次序号，旧身份不会与新身份相同；针对旧会话序号的
//      迟到取消被拒绝且没有任何副作用。
//   7. 幂等退出：重复 shutdown 成功返回且不改变状态；退出后一切入口被明确拒绝。
//
// 夹具全部走显式闸门，不用睡眠决定先后：用例先把某个阶段的门关上，等“拥有者已经进入该
// 阶段”的条件变量通知到达后再发起控制调用，因此判定与机器快慢无关；所有等待都有上界，
// 闸门不放行时会变成明确失败而不是把测试挂住。
//
// 本文件不引入模型、声卡、网络或子进程依赖：唯一的“外部资源”就是夹具拥有者与它自己的
// 线程，两者都由本文件创建与释放。
#include "../test_support.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "supervisor.hpp"

using namespace nexweave;
using nexweave::domain::ErrorCode;
using nexweave::runtime::ISessionOwner;
using nexweave::runtime::ISessionOwnerFactory;
using nexweave::runtime::Supervisor;
using nexweave::runtime::SupervisorConfig;
using nexweave::runtime::SupervisorControlOutcome;
using nexweave::runtime::SupervisorSessionSpec;
using nexweave::runtime::SupervisorState;
using nexweave::runtime::SupervisorStatus;

namespace {

using Milliseconds = std::chrono::milliseconds;

// 一个足够长的等待上界：它只用于“等待某个已经确定会发生的事件”，不用于表达业务超时。
// 取值远大于任何夹具的工作时间，因此在正常机器上永远不会真正等满；等满即说明实现有缺陷。
constexpr Milliseconds kWaitBound{4000};

// 一个足够短的预算：用于制造“预算内没有收敛”的情形。夹具的闸门在用例放行前一直关着，
// 因此超时是确定性事件，而不是与机器速度赛跑。
constexpr Milliseconds kTinyBudget{20};

// 夹具拥有者的闸门模板：工厂为每一次会话按这份模板创建新拥有者，使用例可以在 start()
// 之前就决定“哪个阶段会阻塞、哪个阶段会失败”。
struct OwnerGate {
  bool hold_start = false;
  bool hold_run = false;
  bool hold_cleanup = false;
  domain::Error start_error{};
  domain::Error run_error{};
  domain::Error cleanup_error{};

  static OwnerGate FailingAtCleanup(ErrorCode code) {
    OwnerGate gate;
    gate.cleanup_error = domain::Error{code, "夹具注入的清理失败"};
    return gate;
  }
};

// 确定性拥有者夹具：把 start/run/cleanup 三个阶段各自做成一个显式闸门。
//
// 线程约定：三个阶段由监督器的工作线程调用，闸门由测试线程开关，全部状态受一把互斥量保护；
// 停止计数是原子的，因为它在控制线程上被读取而由工作线程写入（多数情况下是同一线程，但
// 契约不要求这一点）。
//
// 资源归属：本对象不创建线程、文件或设备句柄，只持有一个条件变量与几个计数；它的生命周期
// 由测试与监督器共同持有（双方各有一份 shared_ptr），因此监督器析构不会让工作线程访问到
// 已释放的夹具。
class GatedOwner final : public ISessionOwner {
 public:
  explicit GatedOwner(OwnerGate gate) : gate_(std::move(gate)) {}

  domain::OperationResult start() override {
    std::unique_lock<std::mutex> lock(mutex_);
    start_entered_ = true;
    ++start_calls_;
    cv_.notify_all();
    if (gate_.hold_start) {
      cv_.wait(lock, [this] { return released_start_; });
    }
    return domain::OperationResult{gate_.start_error};
  }

  domain::OperationResult run() override {
    std::unique_lock<std::mutex> lock(mutex_);
    run_entered_ = true;
    ++run_calls_;
    cv_.notify_all();
    if (gate_.hold_run) {
      cv_.wait(lock, [this] { return released_run_; });
    }
    return domain::OperationResult{gate_.run_error};
  }

  // cleanup() 是 noexcept 接口：条件变量等待理论上可能抛异常，因此在这里收敛成结构化失败，
  // 而不是让一次测试夹具的内部错误升级成 std::terminate。
  domain::OperationResult cleanup() noexcept override {
    try {
      std::unique_lock<std::mutex> lock(mutex_);
      cleanup_entered_ = true;
      ++cleanup_calls_;
      cv_.notify_all();
      if (gate_.hold_cleanup) {
        cv_.wait(lock, [this] { return released_cleanup_; });
      }
      cleanup_returned_ = true;
      cv_.notify_all();
      return domain::OperationResult{gate_.cleanup_error};
    } catch (...) {
      return domain::OperationResult::failure(ErrorCode::kBackendFailure,
                                              "夹具清理等待失败");
    }
  }

  void request_stop() noexcept override {
    // 只做一次原子计数与置位：接缝要求这个入口非阻塞、不分配、不抛异常。
    stop_requests_.fetch_add(1);
  }

  bool wait_for_start_entered(Milliseconds budget) {
    return wait_for([this] { return start_entered_; }, budget);
  }

  bool wait_for_run_entered(Milliseconds budget) {
    return wait_for([this] { return run_entered_; }, budget);
  }

  bool wait_for_cleanup_entered(Milliseconds budget) {
    return wait_for([this] { return cleanup_entered_; }, budget);
  }

  bool wait_for_cleanup_returned(Milliseconds budget) {
    return wait_for([this] { return cleanup_returned_; }, budget);
  }

  void release_start() {
    release([this] { released_start_ = true; });
  }

  void release_run() {
    release([this] { released_run_ = true; });
  }

  void release_cleanup() {
    release([this] { released_cleanup_ = true; });
  }

  int start_calls() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return start_calls_;
  }

  int run_calls() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return run_calls_;
  }

  int cleanup_calls() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cleanup_calls_;
  }

  int stop_requests() const noexcept {
    return stop_requests_.load();
  }

 private:
  template <typename Predicate>
  bool wait_for(Predicate predicate, Milliseconds budget) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, budget, predicate);
  }

  template <typename Mutation>
  void release(Mutation mutation) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      mutation();
    }
    cv_.notify_all();
  }

  OwnerGate gate_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool start_entered_ = false;
  bool run_entered_ = false;
  bool cleanup_entered_ = false;
  bool cleanup_returned_ = false;
  bool released_start_ = false;
  bool released_run_ = false;
  bool released_cleanup_ = false;
  int start_calls_ = 0;
  int run_calls_ = 0;
  int cleanup_calls_ = 0;
  std::atomic<int> stop_requests_{0};
};

// 拥有者工厂夹具：按当前模板为每次会话产出**全新**的拥有者，并记录每次 create 收到的
// 启动参数副本，使“忙碌拒绝没有偷偷创建会话”“会话序号与 work_id 由监督器回填”可以被断言。
class RecordingOwnerFactory final : public ISessionOwnerFactory {
 public:
  void set_gate(OwnerGate gate) {
    const std::lock_guard<std::mutex> lock(mutex_);
    gate_ = std::move(gate);
  }

  // 让下一次 create 直接失败；失败原因由调用方给出。
  void fail_next_create(domain::Error error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    fail_next_ = true;
    failure_ = std::move(error);
  }

  std::shared_ptr<ISessionOwner> create(const SupervisorSessionSpec& spec,
                                        domain::Error& error) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++create_calls_;
    specs_.push_back(spec);
    if (fail_next_) {
      fail_next_ = false;
      error = failure_;
      return nullptr;
    }
    last_owner_ = std::make_shared<GatedOwner>(gate_);
    return last_owner_;
  }

  std::shared_ptr<GatedOwner> owner() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_owner_;
  }

  // 丢掉工厂自己持有的那份拥有者引用。该用例需要判断“工作线程是否已经交还夹具”，而只要
  // 工厂还留着一份引用，那个判断就永远看不到释放；这里的语义是“工厂不必再为测试保留它”。
  void forget_owner() {
    const std::lock_guard<std::mutex> lock(mutex_);
    last_owner_.reset();
  }

  int create_calls() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return create_calls_;
  }

  std::vector<SupervisorSessionSpec> specs() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return specs_;
  }

 private:
  mutable std::mutex mutex_;
  OwnerGate gate_{};
  bool fail_next_ = false;
  domain::Error failure_{};
  int create_calls_ = 0;
  std::vector<SupervisorSessionSpec> specs_;
  std::shared_ptr<GatedOwner> last_owner_;
};

SupervisorConfig FastConfig() {
  SupervisorConfig config;
  config.start_wait_budget = kWaitBound;
  config.cleanup_wait_budget = kWaitBound;
  return config;
}

SupervisorSessionSpec MakeSpec(const std::string& work_id,
                               const std::string& request_id = "req-1") {
  SupervisorSessionSpec spec;
  spec.work_id = work_id;
  spec.session_id = "sess-1";
  spec.request_id = request_id;
  return spec;
}

// ---- 状态名与配置校验 ----

// to_string 是运行证据与日志的可读入口：它必须覆盖全部状态且对未知值不崩。
void TestStateNamesAndConfigValidation() {
  CHECK(std::string(runtime::to_string(SupervisorState::kIdle)) == "idle");
  CHECK(std::string(runtime::to_string(SupervisorState::kStarting)) == "starting");
  CHECK(std::string(runtime::to_string(SupervisorState::kActive)) == "active");
  CHECK(std::string(runtime::to_string(SupervisorState::kQuiescing)) == "quiescing");
  CHECK(std::string(runtime::to_string(SupervisorState::kUnavailable)) == "unavailable");
  CHECK(std::string(runtime::to_string(SupervisorState::kClosed)) == "closed");
  CHECK(std::string(runtime::to_string(static_cast<SupervisorState>(200))).empty());

  SupervisorConfig config;
  CHECK(runtime::validate_supervisor_config(config).ok());
  config.start_wait_budget = Milliseconds{0};
  CHECK(runtime::validate_supervisor_config(config).error.code == ErrorCode::kInvalidInput);
  config = SupervisorConfig{};
  config.cleanup_wait_budget = Milliseconds{-1};
  CHECK(runtime::validate_supervisor_config(config).error.code == ErrorCode::kInvalidInput);
  // 非法预算不得让构造失败：监督器整份回退到默认值，调用方仍能用它跑完一次会话。
  RecordingOwnerFactory factory;
  Supervisor supervisor(factory, config);
  CHECK(supervisor.status().state == SupervisorState::kIdle);
}

// ---- 创建与单活跃 ----

// 保护不变量 1 与 6：一次创建建立一个会话并占用唯一槽位；会话序号从 1 开始且身份被原样回显。
void TestStartEstablishesSingleActiveSession() {
  RecordingOwnerFactory factory;
  factory.set_gate(OwnerGate{false, true, false, {}, {}, {}});  // 会话停在 run() 里
  Supervisor supervisor(factory, FastConfig());

  const auto started = supervisor.start(MakeSpec("work-alpha", "req-alpha"));
  CHECK(started.ok());

  const SupervisorStatus status = supervisor.status();
  CHECK(status.state == SupervisorState::kActive);
  CHECK(status.session_sequence == 1);
  CHECK(status.work_id == "work-alpha");
  CHECK(status.session_id == "sess-1");
  CHECK(status.request_id == "req-alpha");
  CHECK(status.cancel_accepted == false);
  CHECK(status.slot_reusable() == false);
  CHECK(status.sessions_started == 1);
  CHECK(status.sessions_completed == 0);
  CHECK(status.busy_rejections == 0);
  CHECK(status.last_start_error.ok());

  auto owner = factory.owner();
  CHECK(owner != nullptr);
  CHECK(owner->start_calls() == 1);
  CHECK(owner->wait_for_run_entered(kWaitBound));

  // 自然结束：run() 放行后会话收敛，槽位回到可复用。
  owner->release_run();
  CHECK(supervisor.wait_for_slot(kWaitBound));
  const SupervisorStatus settled = supervisor.status();
  CHECK(settled.state == SupervisorState::kIdle);
  CHECK(settled.slot_reusable());
  CHECK(settled.sessions_completed == 1);
  CHECK(settled.sessions_cancelled == 0);
  CHECK(settled.sessions_failed == 0);
  CHECK(settled.last_run_error.ok());
  CHECK(settled.last_cleanup_error.ok());
  CHECK(owner->cleanup_calls() == 1);
}

// 保护不变量 1、2：槽位被占用时再次创建得到结构化忙碌，且完全没有副作用——不排队、
// 不替换、不产生第二个拥有者、不推进会话序号。语音打断不能靠并发创建绕过这条约束。
void TestCreateWhileActiveReturnsStructuredBusy() {
  RecordingOwnerFactory factory;
  factory.set_gate(OwnerGate{false, true, false, {}, {}, {}});
  Supervisor supervisor(factory, FastConfig());

  CHECK(supervisor.start(MakeSpec("work-first", "req-first")).ok());
  auto owner = factory.owner();
  CHECK(owner->wait_for_run_entered(kWaitBound));

  const auto second = supervisor.start(MakeSpec("work-second", "req-second"));
  CHECK(!second.ok());
  CHECK(second.error.code == ErrorCode::kBusy);
  CHECK(second.error.retryable());  // 忙碌是“稍后再来”，不是永久失败

  const SupervisorStatus status = supervisor.status();
  CHECK(status.state == SupervisorState::kActive);
  CHECK(status.session_sequence == 1);          // 被拒绝的创建没有推进会话序号
  CHECK(status.work_id == "work-first");  // 仍然是被受理的那一次
  CHECK(status.sessions_started == 1);
  CHECK(status.busy_rejections == 1);
  CHECK(factory.create_calls() == 1);   // 忙碌在工厂之前就被拦住，没有建立第二个拥有者
  CHECK(owner->start_calls() == 1);

  owner->release_run();
  CHECK(supervisor.wait_for_slot(kWaitBound));
}

// 保护不变量 1、3：取消已经受理但清理还没完成时，槽位仍然被占用，创建继续得到忙碌。
void TestCreateWhileCleanupInProgressReturnsBusy() {
  RecordingOwnerFactory factory;
  factory.set_gate(OwnerGate{false, true, false, {}, {}, {}});
  Supervisor supervisor(factory, FastConfig());

  CHECK(supervisor.start(MakeSpec("work-cleanup")).ok());
  auto owner = factory.owner();
  CHECK(owner->wait_for_run_entered(kWaitBound));

  // 只受理、不等待：这一调用之后槽位处于“取消清理中”，拥有者仍停在 run()。
  const SupervisorControlOutcome cancelled = supervisor.cancel(0, Milliseconds{0});
  CHECK(cancelled.ok());
  CHECK(cancelled.accepted);
  CHECK(!cancelled.cleanup_completed);
  CHECK(cancelled.state == SupervisorState::kQuiescing);

  const auto blocked = supervisor.start(MakeSpec("work-intruder"));
  CHECK(!blocked.ok());
  CHECK(blocked.error.code == ErrorCode::kBusy);
  CHECK(factory.create_calls() == 1);
  CHECK(supervisor.status().session_sequence == 1);

  owner->release_run();
  CHECK(supervisor.wait_for_slot(kWaitBound));
}

// 保护不变量 3：查询必须能区分工作、取消清理与不可用，三者不能混成一个“忙”。
void TestStatusDistinguishesWorkingQuiescingAndUnavailable() {
  RecordingOwnerFactory factory;
  factory.set_gate(OwnerGate{false, true, false, {}, {}, {}});
  Supervisor supervisor(factory, FastConfig());

  CHECK(supervisor.start(MakeSpec("work-states")).ok());
  auto owner = factory.owner();
  CHECK(owner->wait_for_run_entered(kWaitBound));
  CHECK(supervisor.status().state == SupervisorState::kActive);
  CHECK(supervisor.status().cancel_accepted == false);

  CHECK(supervisor.cancel(0, Milliseconds{0}).accepted);
  const SupervisorStatus quiescing = supervisor.status();
  CHECK(quiescing.state == SupervisorState::kQuiescing);
  CHECK(quiescing.cancel_accepted);          // 取消清理：state 与 cancel_accepted 同时为真
  CHECK(quiescing.slot_reusable() == false);

  owner->release_run();
  CHECK(supervisor.wait_for_slot(kWaitBound));
  CHECK(supervisor.status().state == SupervisorState::kIdle);

  // 第三个状态：清理失败留下不可用槽位。它与“忙碌”不同——不是稍后再来，而是不再受理。
  factory.set_gate(OwnerGate::FailingAtCleanup(ErrorCode::kDeviceFailure));
  CHECK(supervisor.start(MakeSpec("work-broken")).ok());
  CHECK(supervisor.wait_for_slot(kWaitBound));
  const SupervisorStatus unavailable = supervisor.status();
  CHECK(unavailable.state == SupervisorState::kUnavailable);
  CHECK(unavailable.slot_reusable() == false);
  CHECK(unavailable.last_cleanup_error.code == ErrorCode::kDeviceFailure);
  CHECK(unavailable.sessions_failed == 1);

  const auto rejected = supervisor.start(MakeSpec("work-after-failure"));
  CHECK(!rejected.ok());
  CHECK(rejected.error.code == ErrorCode::kBackendFailure);
  CHECK(factory.create_calls() == 2);
}

// 保护不变量 2、6：work_id 为空时由监督器按会话序号生成；每次新会话得到新序号与新身份，
// 上一次的身份不会被复用（旧身份不会与新身份撞车）。
void TestEachSessionGetsFreshSequenceAndIdentity() {
  RecordingOwnerFactory factory;
  Supervisor supervisor(factory, FastConfig());

  CHECK(supervisor.start(SupervisorSessionSpec{}).ok());
  CHECK(supervisor.status().work_id == "work-1");
  CHECK(supervisor.wait_for_slot(kWaitBound));

  CHECK(supervisor.start(SupervisorSessionSpec{}).ok());
  CHECK(supervisor.status().work_id == "work-2");
  CHECK(supervisor.status().session_sequence == 2);
  CHECK(supervisor.wait_for_slot(kWaitBound));

  const auto specs = factory.specs();
  CHECK(specs.size() == 2);
  CHECK(specs.at(0).session_sequence == 1);
  CHECK(specs.at(1).session_sequence == 2);
  // 工厂拿到的是监督器回填后的身份：它不必自己再推导一次序号。
  CHECK(specs.at(0).work_id == "work-1");
  CHECK(specs.at(1).work_id == "work-2");
  CHECK(factory.create_calls() == 2);
}

// 保护不变量 1、3：工厂拒绝创建时槽位必须完整回到“空闲且已收敛”，而不是留下一个
// 永远不会收敛的空壳——后者会让后续的每次创建都得到忙碌。
void TestFactoryFailureLeavesSlotIdleAndReusable() {
  RecordingOwnerFactory factory;
  factory.fail_next_create(domain::Error{ErrorCode::kBackendFailure, "夹具拒绝创建"});
  Supervisor supervisor(factory, FastConfig());

  const auto refused = supervisor.start(MakeSpec("work-refused"));
  CHECK(!refused.ok());
  CHECK(refused.error.code == ErrorCode::kBackendFailure);

  const SupervisorStatus status = supervisor.status();
  CHECK(status.state == SupervisorState::kIdle);
  CHECK(status.slot_reusable());
  CHECK(status.sessions_started == 0);
  CHECK(status.sessions_failed == 0);  // 没有被受理的创建不落账
  CHECK(status.last_start_error.code == ErrorCode::kBackendFailure);
  CHECK(supervisor.wait_for_slot(kWaitBound));  // 槽位处于已收敛状态，不会阻塞

  // 下一次创建照常受理，且会话序号照常推进。
  CHECK(supervisor.start(MakeSpec("work-retry")).ok());
  CHECK(supervisor.status().session_sequence == 2);
  CHECK(supervisor.wait_for_slot(kWaitBound));
  CHECK(supervisor.status().sessions_completed == 1);
}

// ---- 取消：受理与清理完成是两件事 ----

// 保护不变量 5：只受理不等待时，返回值必须同时表达“取消已受理”和“清理还没完成”，
// 并且把停止传播给拥有者；等待结束后同一个槽位收敛并可复用。
void TestCancelAcceptsImmediatelyThenWaitsForCleanup() {
  RecordingOwnerFactory factory;
  factory.set_gate(OwnerGate{false, true, false, {}, {}, {}});
  Supervisor supervisor(factory, FastConfig());

  CHECK(supervisor.start(MakeSpec("work-cancel")).ok());
  auto owner = factory.owner();
  CHECK(owner->wait_for_run_entered(kWaitBound));

  const SupervisorControlOutcome accepted = supervisor.cancel(0, Milliseconds{0});
  CHECK(accepted.ok());
  CHECK(accepted.accepted);
  CHECK(!accepted.cleanup_completed);
  CHECK(accepted.state == SupervisorState::kQuiescing);
  CHECK(owner->stop_requests() == 1);  // 停止已经传播：拥塞在 run() 里的是拥有者，不是监督器

  // 第二次调用不重复通知拥有者：停止已经受理，重复通知只会给后端增加副作用面。
  const SupervisorControlOutcome again = supervisor.cancel(0, Milliseconds{0});
  CHECK(again.ok());
  CHECK(again.accepted);
  CHECK(!again.cleanup_completed);

  owner->release_run();
  const SupervisorControlOutcome settled = supervisor.cancel(0, kWaitBound);
  CHECK(settled.ok());
  CHECK(settled.accepted);
  CHECK(settled.cleanup_completed);
  CHECK(settled.state == SupervisorState::kIdle);

  const SupervisorStatus status = supervisor.status();
  CHECK(status.sessions_cancelled == 1);
  CHECK(status.sessions_completed == 0);
  CHECK(status.cancel_accepted == false);  // 收敛后复位
}

// 保护不变量 1：空闲时取消是幂等空操作——成功返回、不改状态、不产生终态，也不改变账目。
void TestCancelOnIdleIsIdempotentNoop() {
  RecordingOwnerFactory factory;
  Supervisor supervisor(factory, FastConfig());

  const SupervisorControlOutcome first = supervisor.cancel();
  CHECK(first.ok());
  CHECK(!first.accepted);
  CHECK(first.cleanup_completed);
  CHECK(first.state == SupervisorState::kIdle);

  const SupervisorControlOutcome second = supervisor.cancel();
  CHECK(second.ok());
  CHECK(!second.accepted);
  CHECK(supervisor.status().sessions_cancelled == 0);
  CHECK(factory.create_calls() == 0);
}

// 保护不变量 4：会话主体自然结束、正在收尾时，取消不会改变结果（如实报告未受理），
// 但等待清理照常进行，槽位在清理完成前仍不可复用。
void TestCancelDuringNaturalCleanupIsReportedNotAccepted() {
  RecordingOwnerFactory factory;
  factory.set_gate(OwnerGate{false, false, true, {}, {}, {}});  // 停在 cleanup() 里
  Supervisor supervisor(factory, FastConfig());

  CHECK(supervisor.start(MakeSpec("work-drain")).ok());
  auto owner = factory.owner();
  CHECK(owner->wait_for_cleanup_entered(kWaitBound));

  const SupervisorStatus draining = supervisor.status();
  CHECK(draining.state == SupervisorState::kQuiescing);   // 正在收尾
  CHECK(draining.cancel_accepted == false);               // 但不是因为取消
  CHECK(draining.slot_reusable() == false);

  const SupervisorControlOutcome late = supervisor.cancel(0, Milliseconds{0});
  CHECK(late.ok());
  CHECK(!late.accepted);              // 会话已经在收尾，没有“被打断的工作”可记账
  CHECK(!late.cleanup_completed);
  CHECK(owner->stop_requests() == 0); // 自然收尾不会被伪装成一次取消
  CHECK(supervisor.start(MakeSpec("work-during-drain")).error.code == ErrorCode::kBusy);

  owner->release_cleanup();
  CHECK(supervisor.wait_for_slot(kWaitBound));
  const SupervisorStatus settled = supervisor.status();
  CHECK(settled.sessions_completed == 1);
  CHECK(settled.sessions_cancelled == 0);
}

// ---- 清理、超时与不可用 ----

// 保护不变量 4：清理失败意味着资源状态不明，槽位必须永久不可用，而不是“再试一次就好了”。
// 反复拒绝必须可验证，且后续退出仍然可以完成。
void TestCleanupFailureRetiresSlotPermanently() {
  RecordingOwnerFactory factory;
  factory.set_gate(OwnerGate::FailingAtCleanup(ErrorCode::kDeviceFailure));
  Supervisor supervisor(factory, FastConfig());

  CHECK(supervisor.start(MakeSpec("work-cleanup-fail")).ok());
  CHECK(supervisor.wait_for_slot(kWaitBound));

  const SupervisorStatus status = supervisor.status();
  CHECK(status.state == SupervisorState::kUnavailable);
  CHECK(status.sessions_failed == 1);
  CHECK(status.last_cleanup_error.code == ErrorCode::kDeviceFailure);

  // 不可用槽位对每一个入口都必须明确拒绝：创建、取消都不能“顺手再试一次”。
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto rejected = supervisor.start(MakeSpec("work-again"));
    CHECK(!rejected.ok());
    CHECK(rejected.error.code == ErrorCode::kBackendFailure);
  }
  const SupervisorControlOutcome cancelled = supervisor.cancel();
  CHECK(!cancelled.ok());
  CHECK(cancelled.error.code == ErrorCode::kBackendFailure);
  CHECK(factory.create_calls() == 1);

  // 不可用不等于不能退出：退出仍然完成，但“退没退”与“上一次清理干不干净”分别由 state
  // 与 error 回答——把清理失败的槽位退出时报成一片成功，会让那次失败在证据里消失。
  const SupervisorControlOutcome closed = supervisor.shutdown();
  CHECK(!closed.ok());
  CHECK(closed.error.code == ErrorCode::kDeviceFailure);
  CHECK(closed.cleanup_completed);
  CHECK(closed.state == SupervisorState::kClosed);
  CHECK(supervisor.status().state == SupervisorState::kClosed);
  // 幂等：再退一次成功返回，且状态不变；这次没有新的失败要报告，因此 error 重新为空。
  const SupervisorControlOutcome again = supervisor.shutdown();
  CHECK(again.ok());
  CHECK(!again.accepted);
  CHECK(again.state == SupervisorState::kClosed);
}

// 保护不变量 4、5：清理超出预算时只报告超时，槽位既不复用也不判定为失败；
// 拥有者随后交还资源时槽位正常收敛。
void TestCleanupTimeoutKeepsSlotUnreusableThenSettles() {
  RecordingOwnerFactory factory;
  factory.set_gate(OwnerGate{false, false, true, {}, {}, {}});
  SupervisorConfig config = FastConfig();
  config.cleanup_wait_budget = kTinyBudget;
  Supervisor supervisor(factory, config);

  CHECK(supervisor.start(MakeSpec("work-timeout")).ok());
  auto owner = factory.owner();
  CHECK(owner->wait_for_cleanup_entered(kWaitBound));

  const SupervisorControlOutcome timed_out = supervisor.cancel(0, kTinyBudget);
  CHECK(!timed_out.ok());
  CHECK(timed_out.error.code == ErrorCode::kTimeout);
  CHECK(timed_out.accepted == false);        // 会话已经在自然收尾，本次没有打断任何工作
  CHECK(!timed_out.cleanup_completed);
  CHECK(timed_out.state == SupervisorState::kQuiescing);

  const SupervisorStatus pending = supervisor.status();
  CHECK(pending.state == SupervisorState::kQuiescing);
  CHECK(pending.slot_reusable() == false);   // 清理未完成 ⇒ 不得复用
  CHECK(supervisor.start(MakeSpec("work-blocked")).error.code == ErrorCode::kBusy);

  owner->release_cleanup();
  CHECK(owner->wait_for_cleanup_returned(kWaitBound));
  CHECK(supervisor.wait_for_slot(kWaitBound));
  const SupervisorStatus settled = supervisor.status();
  CHECK(settled.state == SupervisorState::kIdle);
  CHECK(settled.sessions_completed == 1);
  CHECK(settled.sessions_failed == 0);       // 超时本身不是失败终态
}

// 保护不变量 2、4：建立失败必须回滚（仍然调用 cleanup）并把槽位放回可用，
// 使下一次创建能正常受理；失败原因保留在状态快照里可查。
void TestStartFailureRollsBackAndKeepsSlotReusable() {
  RecordingOwnerFactory factory;
  OwnerGate broken;
  broken.start_error = domain::Error{ErrorCode::kDeviceFailure, "夹具注入的建立失败"};
  factory.set_gate(broken);
  Supervisor supervisor(factory, FastConfig());

  const auto failed = supervisor.start(MakeSpec("work-start-fail"));
  CHECK(!failed.ok());
  CHECK(failed.error.code == ErrorCode::kDeviceFailure);

  auto owner = factory.owner();
  CHECK(owner != nullptr);
  CHECK(owner->cleanup_calls() == 1);  // 建立失败也必须走清理，不能把半套资源留在拥有者手里
  CHECK(owner->run_calls() == 0);      // 没有建立成功就没有被执行的会话

  const SupervisorStatus status = supervisor.status();
  CHECK(status.state == SupervisorState::kIdle);
  CHECK(status.slot_reusable());
  CHECK(status.sessions_started == 0);
  CHECK(status.sessions_failed == 1);
  CHECK(status.last_start_error.code == ErrorCode::kDeviceFailure);
  CHECK(status.last_cleanup_error.ok());

  factory.set_gate(OwnerGate{});
  CHECK(supervisor.start(MakeSpec("work-start-ok")).ok());
  CHECK(supervisor.wait_for_slot(kWaitBound));
  CHECK(supervisor.status().sessions_completed == 1);
}

// 保护不变量 4、5：建立超出启动预算时返回超时并把槽位转为不可用——建立过程没有收敛，
// 外部资源状态未知；同时必须把停止传播出去，避免一个无人认领的会话一直跑到自然结束。
void TestStartTimeoutRetiresSlotAndPropagatesStop() {
  RecordingOwnerFactory factory;
  OwnerGate stuck;
  stuck.hold_start = true;
  factory.set_gate(stuck);
  SupervisorConfig config = FastConfig();
  config.start_wait_budget = kTinyBudget;
  Supervisor supervisor(factory, config);

  const auto started = supervisor.start(MakeSpec("work-start-timeout"));
  CHECK(!started.ok());
  CHECK(started.error.code == ErrorCode::kTimeout);

  auto owner = factory.owner();
  CHECK(owner != nullptr);
  CHECK(owner->wait_for_start_entered(kWaitBound));
  CHECK(owner->stop_requests() >= 1);  // 停止不可省：否则这个会话没有任何外部收敛手段

  const SupervisorStatus status = supervisor.status();
  CHECK(status.state == SupervisorState::kUnavailable);
  CHECK(status.slot_reusable() == false);
  CHECK(status.last_start_error.code == ErrorCode::kTimeout);
  CHECK(supervisor.start(MakeSpec("work-after-timeout")).error.code == ErrorCode::kBackendFailure);

  // 放行后拥有者仍然会走完 cleanup，槽位进入“已收敛的不可用”，而不是永远挂着。
  owner->release_start();
  CHECK(supervisor.wait_for_slot(kWaitBound));
  CHECK(owner->run_calls() == 0);  // 调用方已经收到建立失败，不再跑一轮无人认领的会话
  CHECK(supervisor.status().state == SupervisorState::kUnavailable);
}

// 保护不变量 4、7：建立超时留下的槽位虽然已经是“不可用”，但工作线程还阻塞在拥有者的
// start() 里——会话确实在途。此时退出**不能**宣称清理已经在预算内结束，否则“已退出”
// 会被随后收尾的工作线程改回不可用，监督器的终态被自己推翻。
void TestShutdownWhileStartIsStillPendingWaitsForTheOwner() {
  RecordingOwnerFactory factory;
  OwnerGate stuck;
  stuck.hold_start = true;
  factory.set_gate(stuck);
  SupervisorConfig config = FastConfig();
  config.start_wait_budget = kTinyBudget;
  Supervisor supervisor(factory, config);

  CHECK(supervisor.start(MakeSpec("work-stuck-start")).error.code == ErrorCode::kTimeout);
  auto owner = factory.owner();
  CHECK(owner != nullptr);
  CHECK(owner->wait_for_start_entered(kWaitBound));

  // 预算内没有收敛：退出如实超时，状态保持“不可用”且监督器没有宣称已经退出。
  const SupervisorControlOutcome timed_out = supervisor.shutdown(kTinyBudget);
  CHECK(!timed_out.ok());
  CHECK(timed_out.error.code == ErrorCode::kTimeout);
  CHECK(!timed_out.cleanup_completed);
  CHECK(supervisor.status().state != SupervisorState::kClosed);

  // 放行拥有者，让工作线程走完收尾；此后退出才真正完成。
  owner->release_start();
  CHECK(supervisor.wait_for_slot(kWaitBound));
  const SupervisorControlOutcome closed = supervisor.shutdown(kWaitBound);
  CHECK(closed.state == SupervisorState::kClosed);
  CHECK(closed.cleanup_completed);
  CHECK(supervisor.status().state == SupervisorState::kClosed);

  // 终态不会被已经结束的工作线程推翻：无论此后有没有别的动作，它都停在“已退出”。
  CHECK(supervisor.status().state == SupervisorState::kClosed);
  CHECK(supervisor.start(MakeSpec("work-after-stuck")).error.code == ErrorCode::kAlreadyCompleted);
  const SupervisorControlOutcome repeated = supervisor.shutdown();
  CHECK(repeated.ok());
  CHECK(repeated.state == SupervisorState::kClosed);
}

// ---- 退出 ----

// 保护不变量 7：重复退出成功返回且不改变状态；退出后所有入口都被明确拒绝。
void TestShutdownIsIdempotentAndClosesEveryEntry() {
  RecordingOwnerFactory factory;
  Supervisor supervisor(factory, FastConfig());

  const SupervisorControlOutcome first = supervisor.shutdown();
  CHECK(first.ok());
  CHECK(first.accepted);
  CHECK(first.cleanup_completed);
  CHECK(first.state == SupervisorState::kClosed);

  for (int attempt = 0; attempt < 2; ++attempt) {
    const SupervisorControlOutcome repeated = supervisor.shutdown();
    CHECK(repeated.ok());
    CHECK(!repeated.accepted);  // 第二次没有改变任何状态，但仍然成功
    CHECK(repeated.state == SupervisorState::kClosed);
  }

  const auto created = supervisor.start(MakeSpec("work-after-close"));
  CHECK(!created.ok());
  CHECK(created.error.code == ErrorCode::kAlreadyCompleted);
  const SupervisorControlOutcome cancelled = supervisor.cancel();
  CHECK(!cancelled.ok());
  CHECK(cancelled.error.code == ErrorCode::kAlreadyCompleted);
  CHECK(factory.create_calls() == 0);
  CHECK(supervisor.status().state == SupervisorState::kClosed);
}

// 保护不变量 5、7：退出必须先把停止传播给在途会话，再等清理，最后才终结；
// 预算不足时如实超时且保持未退出，重复调用可以继续等——退出不会因一次超时变成不可完成。
void TestShutdownStopsSessionBeforeClosingAndIsRetryable() {
  RecordingOwnerFactory factory;
  factory.set_gate(OwnerGate{false, true, false, {}, {}, {}});
  Supervisor supervisor(factory, FastConfig());

  CHECK(supervisor.start(MakeSpec("work-shutdown")).ok());
  auto owner = factory.owner();
  CHECK(owner->wait_for_run_entered(kWaitBound));

  const SupervisorControlOutcome timed_out = supervisor.shutdown(kTinyBudget);
  CHECK(!timed_out.ok());
  CHECK(timed_out.error.code == ErrorCode::kTimeout);
  CHECK(timed_out.accepted);
  CHECK(!timed_out.cleanup_completed);
  CHECK(timed_out.state == SupervisorState::kQuiescing);
  CHECK(owner->stop_requests() == 1);  // 退出先停止会话，再等清理，而不是直接丢下它
  CHECK(supervisor.status().state == SupervisorState::kQuiescing);
  CHECK(supervisor.start(MakeSpec("work-while-closing")).error.code == ErrorCode::kBusy);

  owner->release_run();
  CHECK(supervisor.wait_for_slot(kWaitBound));

  const SupervisorControlOutcome closed = supervisor.shutdown(kWaitBound);
  CHECK(closed.ok());
  CHECK(closed.accepted);
  CHECK(closed.cleanup_completed);
  CHECK(closed.state == SupervisorState::kClosed);
  CHECK(supervisor.status().state == SupervisorState::kClosed);
  CHECK(supervisor.status().cancel_accepted == false);
}

// ---- 会话序号与迟到请求 ----

// 保护不变量 6：针对上一次会话的迟到取消必须被拒绝且没有任何副作用；新会话仍然可以被
// 正常取消。这条不变量是“重建身份后旧请求不得影响新会话”的进程内形态。
void TestLateCancelForPreviousSequenceIsRejected() {
  RecordingOwnerFactory factory;
  OwnerGate gate;
  gate.hold_run = true;
  factory.set_gate(gate);
  Supervisor supervisor(factory, FastConfig());

  CHECK(supervisor.start(MakeSpec("work-one", "req-one")).ok());
  auto first_owner = factory.owner();
  CHECK(first_owner->wait_for_run_entered(kWaitBound));
  first_owner->release_run();
  CHECK(supervisor.wait_for_slot(kWaitBound));
  CHECK(supervisor.status().session_sequence == 1);

  CHECK(supervisor.start(MakeSpec("work-two", "req-two")).ok());
  auto second_owner = factory.owner();
  CHECK(second_owner->wait_for_run_entered(kWaitBound));
  CHECK(supervisor.status().session_sequence == 2);

  // 迟到的取消：目标会话序号是上一次会话。
  const SupervisorControlOutcome late = supervisor.cancel(1, Milliseconds{0});
  CHECK(!late.ok());
  CHECK(late.error.code == ErrorCode::kInvalidInput);
  CHECK(!late.accepted);
  const SupervisorStatus untouched = supervisor.status();
  CHECK(untouched.state == SupervisorState::kActive);   // 新会话没有被误伤
  CHECK(untouched.cancel_accepted == false);
  CHECK(second_owner->stop_requests() == 0);            // 也没有被误通知

  // 目标会话序号正确时取消照常受理。
  const SupervisorControlOutcome target = supervisor.cancel(2, Milliseconds{0});
  CHECK(target.ok());
  CHECK(target.accepted);
  CHECK(second_owner->stop_requests() == 1);

  second_owner->release_run();
  CHECK(supervisor.wait_for_slot(kWaitBound));
  CHECK(supervisor.status().sessions_completed == 1);
  CHECK(supervisor.status().sessions_cancelled == 1);
}

// 保护线程与资源归属：析构不得因为拥有者迟迟不交还资源而无限阻塞，也不得升级成崩溃；
// 放弃等待之后，那个工作线程仍然必须能自己收敛干净。
//
// 这个用例刻意让一个工作线程活过监督器，因此夹具的存活不能依赖测试栈上的引用：工作线程
// 自己持有一份槽位与一份夹具拥有者的共享指针，所以即使测试把它们全部释放，对象也只会由
// 最后一个持有者销毁。判据取“夹具已经没人引用”，因为工作线程在收尾时释放它对夹具的最后
// 一份引用，那一刻就是它最后一次触碰测试夹具——此后它只剩槽位的收尾，与测试对象无关。
void TestDestructorStopsWaitingWithinBudget() {
  RecordingOwnerFactory factory;
  factory.set_gate(OwnerGate{false, false, true, {}, {}, {}});
  std::shared_ptr<GatedOwner> owner;
  {
    SupervisorConfig config = FastConfig();
    config.cleanup_wait_budget = kTinyBudget;
    Supervisor supervisor(factory, config);
    CHECK(supervisor.start(MakeSpec("work-destruct")).ok());
    owner = factory.owner();
    CHECK(owner->wait_for_cleanup_entered(kWaitBound));
    // 作用域结束时析构在预算内返回：闸门仍然关着，因此清理不可能已经完成。
  }
  CHECK(owner->stop_requests() >= 1);  // 析构仍然把停止传播出去了

  // 放行清理，然后丢掉测试自己持有的两份引用（工厂那份随函数结束销毁）。此后夹具只剩
  // 工作线程还引用着；等它消失，就等于工作线程已经收尾完毕，进程退出时不再有游离线程。
  owner->release_cleanup();
  CHECK(owner->wait_for_cleanup_returned(kWaitBound));
  const std::weak_ptr<GatedOwner> observer = owner;
  factory.forget_owner();
  owner.reset();

  const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
  while (!observer.expired() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(Milliseconds{1});
  }
  CHECK(observer.expired());
}

}  // namespace

int main() {
  try {
    TestStateNamesAndConfigValidation();
    TestStartEstablishesSingleActiveSession();
    TestCreateWhileActiveReturnsStructuredBusy();
    TestCreateWhileCleanupInProgressReturnsBusy();
    TestStatusDistinguishesWorkingQuiescingAndUnavailable();
    TestEachSessionGetsFreshSequenceAndIdentity();
    TestFactoryFailureLeavesSlotIdleAndReusable();
    TestCancelAcceptsImmediatelyThenWaitsForCleanup();
    TestCancelOnIdleIsIdempotentNoop();
    TestCancelDuringNaturalCleanupIsReportedNotAccepted();
    TestCleanupFailureRetiresSlotPermanently();
    TestCleanupTimeoutKeepsSlotUnreusableThenSettles();
    TestStartFailureRollsBackAndKeepsSlotReusable();
    TestStartTimeoutRetiresSlotAndPropagatesStop();
    TestShutdownWhileStartIsStillPendingWaitsForTheOwner();
    TestShutdownIsIdempotentAndClosesEveryEntry();
    TestShutdownStopsSessionBeforeClosingAndIsRetryable();
    TestLateCancelForPreviousSequenceIsRejected();
    TestDestructorStopsWaitingWithinBudget();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "supervisor 生命周期用例未通过: %s\n", error.what());
    return 1;
  }
  return 0;
}

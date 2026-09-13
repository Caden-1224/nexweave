#include "supervisor.hpp"

#include <utility>

namespace nexweave::runtime {

namespace {

// 构造一次“操作被拒绝”的控制结果：没有任何状态被改变，因此 accepted 与 cleanup_completed
// 都为假，state 原样回显调用瞬间的快照，错误码与文本由调用点给出。
SupervisorControlOutcome make_rejection(domain::ErrorCode code, std::string message,
                                        SupervisorState state) {
  SupervisorControlOutcome outcome;
  outcome.state = state;
  outcome.error = domain::OperationResult::failure(code, std::move(message)).error;
  return outcome;
}

// 把“等待清理超出预算”统一成同一个错误值：三条等待路径（cancel、shutdown、建立回滚）都用
// 它，避免同一个事实在不同入口给出不同的错误码或文本。
domain::Error make_wait_timeout() {
  return domain::OperationResult::failure(domain::ErrorCode::kTimeout,
                                          "等待会话清理超出声明预算，槽位仍不可复用")
      .error;
}

// 槽位会话序号达到上限时的保护：继续自增会回绕，从而让旧身份与新身份相同（ABA）。本版在
// 上限处拒绝受理，而不是回绕或抛异常。
constexpr std::uint64_t kMaxSessionSequence = ~static_cast<std::uint64_t>(0);

}  // namespace

// 槽位：监督器与工作线程共享的可变状态。全部字段都由 mutex 保护，cv 只用于两类等待：
// “建立是否已经返回”与“清理是否已经结束”。
//
// 不变量的时序（都在同一把锁下提交，因此观察者不会看到中间态）：
//   - state 为 kStarting/kActive 时，owner 非空且 finished 为假；
//   - state 为 kQuiescing 时，finished 仍为假（资源未交还）；stop_accepted 说明这次收尾
//     是被取消打断的，还是会话主体自己结束的；
//   - state 为 kIdle/kUnavailable 时，finished 为真且 owner 已经由工作线程释放；
//   - 因此“槽位可复用”等价于 “state == kIdle”，而不是“某个人说清理完了”。
struct Supervisor::Slot {
  mutable std::mutex mutex;
  std::condition_variable cv;

  SupervisorState state = SupervisorState::kIdle;
  // 会话序号：每次被受理的创建 +1，只增不减、不回收复用。0 表示从未受理过创建。
  std::uint64_t session_sequence = 0;
  // 当前（或最近一次）会话的身份。收敛后保留，供证据核对“刚结束的是哪一次”。
  SupervisorSessionSpec spec{};

  // 本次会话的拥有者。由监督器在 start() 的调用线程上写入，之后只由工作线程读取与释放。
  // 非空期间它一定还活着，因此持锁调用 owner->request_stop() 不会取到过期指针。
  std::shared_ptr<ISessionOwner> owner{};

  // 拥有者的 start() 是否已经返回；它是 start() 的等待谓词。
  bool start_ready = true;
  // 监督器是否已经放弃这次会话（建立超时）。置位后槽位不再回到可用，且工作线程不再执行
  // run()：调用方已经被答复“建立失败”，此时再跑一轮无人认领的会话没有意义。
  bool start_retired = false;
  // 拥有者是否已经把资源交还（或从未建立过会话）。初始为真：空闲槽位天然是“已收敛”的。
  bool finished = true;
  // 本次会话是否受理过停止。它决定会话收敛时记入 cancelled 还是 completed；每次新会话复位。
  bool stop_accepted = false;

  domain::Error last_start_error{};
  domain::Error last_run_error{};
  domain::Error last_cleanup_error{};

  std::uint64_t sessions_started = 0;
  std::uint64_t sessions_completed = 0;
  std::uint64_t sessions_cancelled = 0;
  std::uint64_t sessions_failed = 0;
  std::uint64_t busy_rejections = 0;
};

const char* to_string(SupervisorState state) noexcept {
  switch (state) {
    case SupervisorState::kIdle:
      return "idle";
    case SupervisorState::kStarting:
      return "starting";
    case SupervisorState::kActive:
      return "active";
    case SupervisorState::kQuiescing:
      return "quiescing";
    case SupervisorState::kUnavailable:
      return "unavailable";
    case SupervisorState::kClosed:
      return "closed";
  }
  return "";
}

domain::OperationResult validate_supervisor_config(const SupervisorConfig& config) {
  if (config.start_wait_budget <= std::chrono::milliseconds::zero()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "start_wait_budget 必须为正");
  }
  if (config.cleanup_wait_budget <= std::chrono::milliseconds::zero()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "cleanup_wait_budget 必须为正");
  }
  return domain::OperationResult::success();
}

Supervisor::Supervisor(ISessionOwnerFactory& factory, SupervisorConfig config)
    : factory_(factory), config_(config) {
  // 非法预算整份回退到默认值，而不是让构造失败：与既有的确定性夹具一致（非法帧时长回退到
  // 固定帧时长），使“构造不会抛业务异常”这条约定对调用方始终成立。希望把配置错误当错误
  // 处理的调用方先调用 validate_supervisor_config()。
  if (!validate_supervisor_config(config_).ok()) {
    config_ = SupervisorConfig{};
  }
  slot_ = std::make_shared<Slot>();
}

Supervisor::~Supervisor() {
  // 析构的前提是调用方不再并发调用本对象的任何方法；因此这里可以安全地直接读写成员。
  {
    const std::lock_guard<std::mutex> guard(slot_->mutex);
    if (slot_->state == SupervisorState::kStarting || slot_->state == SupervisorState::kActive) {
      // 在途会话：受理停止。request_stop 的契约保证它非阻塞、不分配、不抛异常。
      accept_stop_locked();
    } else if (slot_->state == SupervisorState::kQuiescing) {
      // 已经在清理：停止此前已经受理过，这里只是把“会话该结束了”再说一次。
      if (slot_->owner != nullptr) {
        slot_->owner->request_stop();
      }
    }
  }

  if (!worker_.joinable()) {
    return;
  }
  bool settled = false;
  {
    std::unique_lock<std::mutex> lock(slot_->mutex);
    settled = slot_->cv.wait_for(lock, config_.cleanup_wait_budget,
                                [this] { return slot_->finished; });
  }
  if (settled) {
    // 清理已经结束：线程即将返回，join 不会等待任何实质工作。
    worker_.join();
  } else {
    // 预算内没有收敛：放弃等待并 detach。工作线程只持有槽位的 shared_ptr，不持有本对象，
    // 因此 detach 之后不存在悬空访问；代价是那个槽位与线程会一直存在到清理自己结束。
    // 之所以不无限等待：析构可能发生在信号或错误路径上，把它变成不可中断的阻塞点会让
    // “退出”本身失去上界；之所以不 std::terminate：那会把一次清理超时升级成进程崩溃。
    worker_.detach();
  }
}

domain::OperationResult Supervisor::start(const SupervisorSessionSpec& spec) {
  SupervisorSessionSpec effective = spec;
  {
    const std::lock_guard<std::mutex> guard(slot_->mutex);
    switch (slot_->state) {
      case SupervisorState::kStarting:
      case SupervisorState::kActive:
      case SupervisorState::kQuiescing:
        // 忙碌：不排队、不替换、不并发跑第二个会话。计数在拒绝时落账，使“忙碌拒绝没有偷偷
        // 创建会话”可以被外部核对。
        slot_->busy_rejections += 1;
        return domain::OperationResult::failure(
            domain::ErrorCode::kBusy, "已有会话占用唯一活跃槽位，本次创建未被受理");
      case SupervisorState::kUnavailable:
        return domain::OperationResult::failure(
            domain::ErrorCode::kBackendFailure, "槽位在上一次会话中未能完成清理，不再复用");
      case SupervisorState::kClosed:
        return domain::OperationResult::failure(domain::ErrorCode::kAlreadyCompleted,
                                                "监督器已退出，不再受理创建");
      case SupervisorState::kIdle:
        break;
    }
    if (slot_->session_sequence == kMaxSessionSequence) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "会话序号已达上限，无法在不回绕的前提下创建");
    }
    // 受理：在同一临界区里占住槽位并推进会话序号，使并发的第二次创建在锁外只能看到忙碌，
    // 而不是与本次同时通过检查。会话序号只增不减、不回收，因此旧身份不会与新身份相同。
    slot_->session_sequence += 1;
    effective.session_sequence = slot_->session_sequence;
    if (effective.work_id.empty()) {
      effective.work_id = "work-" + std::to_string(effective.session_sequence);
    }
    slot_->spec = effective;
    slot_->state = SupervisorState::kStarting;
    slot_->start_ready = false;
    slot_->start_retired = false;
    slot_->finished = false;
    slot_->stop_accepted = false;
    slot_->last_start_error = domain::Error{};
    slot_->last_run_error = domain::Error{};
    slot_->last_cleanup_error = domain::Error{};
  }

  // 上一个工作线程此时一定已经结束（kIdle 蕴含 finished），先回收它再建立新的。
  // 这一步在锁外做，避免持锁 join；由于槽位已进入 kStarting，并发的创建只会得到忙碌。
  if (worker_.joinable()) {
    worker_.join();
  }

  // 拥有者在调用线程上创建：工厂失败必须能被同步报告，而且失败时还没有工作线程需要回收。
  domain::Error create_error{};
  std::shared_ptr<ISessionOwner> owner = factory_.create(effective, create_error);
  if (owner == nullptr) {
    const std::lock_guard<std::mutex> guard(slot_->mutex);
    // 本次没有建立任何资源、也没有工作线程，因此槽位必须完整回到“空闲且已收敛”的状态：
    // finished 若留在假值上，后续的 wait_for_slot 会永远等一个不存在的会话。
    slot_->state = SupervisorState::kIdle;
    slot_->finished = true;
    slot_->spec = SupervisorSessionSpec{};
    if (create_error.ok()) {
      // 工厂既没给拥有者也没给原因：补一个明确的后端错误，而不是让失败看起来像成功。
      create_error = domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                                      "拥有者工厂未创建会话且未给出失败原因")
                         .error;
    }
    slot_->last_start_error = create_error;
    return domain::OperationResult{create_error};
  }

  {
    const std::lock_guard<std::mutex> guard(slot_->mutex);
    slot_->owner = owner;
  }
  // 线程只捕获槽位与拥有者的 shared_ptr，不捕获 this：析构 detach 因此不会留下指向
  // 已销毁监督器的访问路径。
  worker_ = std::thread(&Supervisor::run_session, slot_, owner);

  std::unique_lock<std::mutex> lock(slot_->mutex);
  const bool ready =
      slot_->cv.wait_for(lock, config_.start_wait_budget, [this] { return slot_->start_ready; });
  if (!ready) {
    // 建立迟迟不返回：外部资源状态未知，槽位转为不可用是唯一保守的处理。仍然把停止传播
    // 出去，否则这个已经无人认领的会话会一直跑到自然结束，而外部没有任何手段让它收敛。
    slot_->start_retired = true;
    if (slot_->owner != nullptr) {
      slot_->owner->request_stop();
    }
    slot_->state = SupervisorState::kUnavailable;
    const domain::Error error =
        domain::OperationResult::failure(domain::ErrorCode::kTimeout,
                                         "会话建立超出启动预算，槽位转为不可用")
            .error;
    slot_->last_start_error = error;
    return domain::OperationResult{error};
  }
  if (!slot_->last_start_error.ok()) {
    // 建立失败：拥有者随后仍会执行 cleanup() 回滚已建立的部分资源。等待回滚结束再答复，
    // 使调用方拿到的“失败”是收敛后的失败，而不是一个“资源可能还在”的中间态。
    // 等待期间互斥量由 wait_for 释放，因此工作线程能在 cleanup() 返回后拿锁提交 finished；
    // 这里复用的是上面那把已经持有的锁，不会出现“先解锁再重新加锁”的空档。
    const domain::Error start_error = slot_->last_start_error;
    const bool rolled_back = slot_->cv.wait_for(lock, config_.cleanup_wait_budget,
                                                [this] { return slot_->finished; });
    if (!rolled_back) {
      slot_->start_retired = true;
      slot_->state = SupervisorState::kUnavailable;
      return domain::OperationResult{make_wait_timeout()};
    }
    return domain::OperationResult{start_error};
  }
  return domain::OperationResult::success();
}

SupervisorControlOutcome Supervisor::cancel(std::uint64_t expected_session_sequence,
                                            std::optional<std::chrono::milliseconds> wait_budget) {
  const std::chrono::milliseconds budget = effective_wait(wait_budget);
  std::optional<SupervisorControlOutcome> immediate;
  // 本次调用结束时“停止是否处于已受理状态”。它在锁内一次性定下来，因为会话一旦收敛，
  // 槽位上的 stop_accepted 就会被复位，锁外再读既可能是数据竞争也可能读到已经翻页的事实。
  bool accepted = false;
  {
    const std::lock_guard<std::mutex> guard(slot_->mutex);
    if (slot_->state == SupervisorState::kClosed) {
      immediate = make_rejection(domain::ErrorCode::kAlreadyCompleted,
                                 "监督器已退出，没有可取消的会话", slot_->state);
    } else if (slot_->state == SupervisorState::kUnavailable) {
      immediate = make_rejection(domain::ErrorCode::kBackendFailure,
                                 "槽位在上一次会话中未能完成清理，无法确认取消语义",
                                 slot_->state);
    } else if (expected_session_sequence != 0 &&
               expected_session_sequence != slot_->session_sequence) {
      // 目标会话序号不是当前会话：这是一次迟到的取消（例如针对上一次会话）。返回错误而不是
      // 作用到新会话上，否则“重建身份后旧请求不得影响新会话”这条不变量就不成立。
      immediate = make_rejection(domain::ErrorCode::kInvalidInput,
                                 "目标会话序号不是当前会话的序号，取消未作用于任何会话",
                                 slot_->state);
    } else if (slot_->state == SupervisorState::kIdle) {
      // 空闲：没有活跃会话。取消是幂等空操作，成功返回且不产生任何状态迁移。
      SupervisorControlOutcome outcome;
      outcome.accepted = false;
      outcome.cleanup_completed = true;
      outcome.state = slot_->state;
      immediate = outcome;
    } else {
      // kStarting/kActive 推进到 kQuiescing 并通知拥有者；已经处于清理中时不重复通知，
      // 因为停止要么此前已受理、要么会话主体已经自然结束，两种情况下重复 request_stop
      // 都只会给后端增加与取消无关的副作用面。等待清理照常进行，accepted 如实报告。
      accepted = slot_->stop_accepted || accept_stop_locked();
    }
  }
  if (immediate.has_value()) {
    return *immediate;
  }
  SupervisorControlOutcome outcome = await_settled(budget);
  outcome.accepted = accepted;
  return outcome;
}

SupervisorControlOutcome Supervisor::shutdown(
    std::optional<std::chrono::milliseconds> wait_budget) {
  const std::chrono::milliseconds budget = effective_wait(wait_budget);
  std::optional<SupervisorControlOutcome> immediate;
  {
    const std::lock_guard<std::mutex> guard(slot_->mutex);
    if (slot_->state == SupervisorState::kClosed) {
      // 幂等：已经退出，成功返回且不改变任何状态。
      SupervisorControlOutcome outcome;
      outcome.accepted = false;
      outcome.cleanup_completed = true;
      outcome.state = SupervisorState::kClosed;
      immediate = outcome;
    } else if (slot_->finished) {
      // 判据是“拥有者已经交还资源”，**不是**“状态看起来像空闲或不可用”。这两者在建立超时
      // 的路径上会分叉：那时槽位已经是 kUnavailable，但工作线程还阻塞在拥有者的 start() 里，
      // 会话确实在途。用状态判断会让退出在清理根本没发生时就宣称“清理已在预算内结束”，
      // 而且工作线程收尾时还会把 kClosed 改回 kUnavailable，让已退出的监督器复活。
      // 如果槽位是被清理失败留下的，就把那次失败一并带出：同一个事实（上一次清理没成功）
      // 不能因为“这次是退出而不是取消”就消失。
      slot_->state = SupervisorState::kClosed;
      SupervisorControlOutcome outcome;
      outcome.accepted = true;
      outcome.cleanup_completed = true;
      outcome.state = SupervisorState::kClosed;
      outcome.error = slot_->last_cleanup_error;
      immediate = outcome;
    } else {
      accept_stop_locked();
    }
  }
  if (immediate.has_value()) {
    return *immediate;
  }

  SupervisorControlOutcome outcome = await_settled(budget);
  outcome.accepted = true;
  const std::lock_guard<std::mutex> guard(slot_->mutex);
  if (slot_->finished) {
    // 清理已经结束：退出可以完成。清理本身失败时错误仍然如实保留在 outcome 里，于是
    // “退没退”由 state 回答、“上一次清理干不干净”由 error 回答，两者不会互相掩盖。
    slot_->state = SupervisorState::kClosed;
    outcome.state = SupervisorState::kClosed;
    outcome.cleanup_completed = true;
  } else {
    // 预算内没有收敛：保持清理中。重复调用会继续等待，因此退出不会因为一次超时变成
    // 不可完成的动作，槽位也不会在这期间被当成可复用。
    outcome.state = slot_->state;
    outcome.cleanup_completed = false;
    outcome.error = make_wait_timeout();
  }
  return outcome;
}

SupervisorStatus Supervisor::status() const {
  const std::lock_guard<std::mutex> guard(slot_->mutex);
  SupervisorStatus snapshot;
  snapshot.state = slot_->state;
  snapshot.session_sequence = slot_->session_sequence;
  snapshot.work_id = slot_->spec.work_id;
  snapshot.session_id = slot_->spec.session_id;
  snapshot.request_id = slot_->spec.request_id;
  snapshot.cancel_accepted = slot_->stop_accepted;
  snapshot.sessions_started = slot_->sessions_started;
  snapshot.sessions_completed = slot_->sessions_completed;
  snapshot.sessions_cancelled = slot_->sessions_cancelled;
  snapshot.sessions_failed = slot_->sessions_failed;
  snapshot.busy_rejections = slot_->busy_rejections;
  snapshot.last_start_error = slot_->last_start_error;
  snapshot.last_run_error = slot_->last_run_error;
  snapshot.last_cleanup_error = slot_->last_cleanup_error;
  return snapshot;
}

bool Supervisor::wait_for_slot(std::chrono::milliseconds budget) const {
  std::unique_lock<std::mutex> lock(slot_->mutex);
  if (budget <= std::chrono::milliseconds::zero()) {
    return slot_->finished;
  }
  return slot_->cv.wait_for(lock, budget, [this] { return slot_->finished; });
}

bool Supervisor::accept_stop_locked() {
  if (slot_->state != SupervisorState::kStarting && slot_->state != SupervisorState::kActive) {
    // 已经在清理（停止此前已受理，或会话主体已自然结束）或根本没有在途会话：本次调用
    // 没有改变任何状态，因此如实返回 false。
    return false;
  }
  slot_->state = SupervisorState::kQuiescing;
  slot_->stop_accepted = true;
  if (slot_->owner != nullptr) {
    // 在持锁状态下调用：request_stop 的契约保证它非阻塞、不分配、不抛异常，因此不会把
    // 这把锁变成一个可能长时间不可用的点；反过来，把调用移到锁外就需要先取一份拥有者
    // 指针，而那份指针可能在解锁后被工作线程释放。
    slot_->owner->request_stop();
  }
  return true;
}

SupervisorControlOutcome Supervisor::await_settled(std::chrono::milliseconds budget) {
  std::unique_lock<std::mutex> lock(slot_->mutex);
  // budget 为 0 时 wait_for 先求值谓词再立刻返回，因此“只受理不等待”不会引入任何阻塞。
  const bool snapshot_only = budget <= std::chrono::milliseconds::zero();
  const bool settled = slot_->cv.wait_for(lock, budget, [this] { return slot_->finished; });
  SupervisorControlOutcome outcome;
  outcome.cleanup_completed = settled;
  outcome.state = slot_->state;
  if (settled) {
    // 清理结束：把清理本身的失败如实带出。终态不变（会话确实已经收敛），但“资源已经
    // 干净交还”这一承诺是否兑现，由这个错误回答。
    outcome.error = slot_->last_cleanup_error;
  } else if (snapshot_only) {
    // 调用方要的只是“现在是什么情况”：没有等待就没有超时可言，把“没等”报成 kTimeout
    // 会让调用方误以为预算被用尽，也会让“只受理”这条用法失去意义。
    outcome.error = domain::Error{};
  } else {
    outcome.error = make_wait_timeout();
  }
  return outcome;
}

std::chrono::milliseconds Supervisor::effective_wait(
    const std::optional<std::chrono::milliseconds>& budget) const {
  if (!budget.has_value()) {
    return config_.cleanup_wait_budget;
  }
  if (budget.value() < std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  return budget.value();
}

void Supervisor::run_session(const std::shared_ptr<Slot>& slot,
                             const std::shared_ptr<ISessionOwner>& owner) {
  // 三段调用严格串行、且都在本线程上：拥有者因此不需要为这三个入口加锁。start() 与
  // cleanup() 都在锁外调用，因为它们可能阻塞；锁只用来提交状态，绝不跨越阻塞调用。
  const domain::OperationResult started = owner->start();
  bool skip_run = false;
  {
    const std::lock_guard<std::mutex> guard(slot->mutex);
    slot->start_ready = true;
    slot->last_start_error = started.error;
    if (!started.error.ok() || slot->start_retired) {
      // 建立失败，或监督器已经因为建立超时把这次会话退休：两种情况都不该再跑一轮
      // 无人认领的会话，直接进入清理。
      skip_run = true;
    } else {
      slot->state = SupervisorState::kActive;
      slot->sessions_started += 1;
    }
    slot->cv.notify_all();
  }

  if (!skip_run) {
    const domain::OperationResult ran = owner->run();
    const std::lock_guard<std::mutex> guard(slot->mutex);
    slot->last_run_error = ran.error;
    // 会话主体结束即进入清理阶段：资源还在自己手里，槽位仍然不可复用。这一步让“正在收尾”
    // 与“正在工作”在状态上可区分；如果停止此前已经受理，状态不会从 kQuiescing 退回。
    if (slot->state == SupervisorState::kActive) {
      slot->state = SupervisorState::kQuiescing;
    }
    slot->cv.notify_all();
  }

  const domain::OperationResult cleaned = owner->cleanup();
  {
    const std::lock_guard<std::mutex> guard(slot->mutex);
    slot->last_cleanup_error = cleaned.error;
    slot->finished = true;

    // 账目：每一次被受理的创建恰好落入 completed / cancelled / failed 之一，使三者和等于
    // 被受理的创建次数（它包含建立失败与建立超时，因此不等于 sessions_started）。被退休的
    // 会话记成失败——调用方拿到的答复就是建立失败，把它记成完成会让账目与外部观测不一致。
    const bool established = started.error.ok() && !slot->start_retired;
    if (!established || !cleaned.error.ok()) {
      slot->sessions_failed += 1;
    } else if (slot->stop_accepted) {
      slot->sessions_cancelled += 1;
    } else if (slot->last_run_error.ok()) {
      slot->sessions_completed += 1;
    } else {
      slot->sessions_failed += 1;
    }

    // 只有清理成功、且这次会话没有被退休，槽位才回到空闲。清理失败意味着资源状态不明，
    // 把它标成可复用会让下一个会话踩在未知状态上；退休意味着建立过程本身没有收敛。
    //
    // 这里不会覆盖 kClosed：退出只在观察到 finished 之后才把状态推进到已退出，而 finished
    // 正是本临界区刚刚提交的；提交之后的代码不再触碰状态，因此“已退出”一旦成立就是终态。
    // 这条顺序是刻意的——先提交 finished 再置终态，观察者要么看到“还在清理”，要么看到
    // “已经终结”，不会看到“终结了但清理还没结束”这种自相矛盾的组合。
    if (slot->start_retired || !cleaned.error.ok()) {
      slot->state = SupervisorState::kUnavailable;
    } else {
      slot->state = SupervisorState::kIdle;
    }
    // 释放拥有者：本线程是唯一使用过它的线程。同时复位本轮的停止标记，避免下一次会话
    // 继承“已受理停止”的历史。spec 刻意保留，使“刚结束的是哪一次”仍可在证据里查到。
    slot->owner.reset();
    slot->stop_accepted = false;
    slot->cv.notify_all();
  }
}

}  // namespace nexweave::runtime

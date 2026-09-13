// 进程内 Gateway 请求入口的集成夹具：验收“外部字节 → 控制操作 → 事件与终态”的完整行为。
//
// 分工：会话生命周期本身由 supervisor_lifecycle_test.cpp 验收，会话编排由 session_*_test.cpp
// 验收。本文件只回答入口自己的问题——字节流怎么切、请求怎么路由、受理与终态怎么分开、
// 重复请求会不会重复启动任务、连接断开与慢客户端怎么收敛、输出会不会互相污染。
//
// 两种会话来源刻意并存：
//   - 闸门化的脚本会话：run() 停在闸门上，使“生成仍在进行”成为一个可复现、可断言的窗口。
//     控制并发、忙碌、取消受理、慢客户端与断连都靠它精确验证，不依赖真实时序。
//   - 真实会话应用：文本模式跑通“识别 → 路由 → 直答 → 合成 → 播放”，用于证明运行记录到
//     数据面事件的映射在真实会话上成立，而不是只在替身上成立。
//
// 保护的不变量（每条在断言旁注明）：
//   1. 受理与终态分开：start 的响应只报告受理，会话的成功/失败/取消由随后 end=true 的终态
//      事件回答；两者不会挤在同一个响应里互相冒充。
//   2. 推理期间控制可用：生成停在闸门上时，查询仍被答复、第二次创建得到结构化忙碌拒绝且
//      真的没有建立第二个会话。
//   3. 取消不得伪装成静默：取消响应报告“受理 + 清理快照”，终态另行报告；清理超时是显式的
//      kTimeout，而不是成功。
//   4. 切分无关：同一请求逐字节喂入与整帧喂入得到同样的响应；一个批次里的多个帧各自成答。
//   5. 幂等：重复 request_id 不会启动第二个任务，执行中返回 kAlreadyCompleted，完成后重放
//      原响应并标记 replayed，字段冲突返回 kInvalidInput。
//   6. 非法输入有稳定结果：能归属到请求的非法输入得到结构化错误且连接保持可用；无法归属的
//      输入没有可寻址的答复对象，因此记录原因并关闭连接，而不是伪造身份回复。
//   7. 有界：单帧超长、待发缓冲达到上限都导致关闭，缓冲不会随输入或输出无界增长。
//   8. 断连策略明确：只有发起会话的那条连接断开才受理取消；断连后的终态没有接收者，被记入
//      丢弃账目而不是静默消失。
//   9. 无残留与确定性：会话事件按会话序号核对，上一次会话的记录不会被当成本次输出；同一
//      场景重复执行得到逐字节一致的输出。
//  10. 上一次会话的结果先交付再开新会话：会话已经收敛但结果还没送出去时，新的创建不得把它
//      顶掉，否则那批事件会消失、那一次创建会永远停在“执行中”。
//  11. 请求自带的预算会收紧本入口自己的等待：配置里的等待上界不能超过客户端声明的预算。
//  12. 幂等记录容量是硬上限：达到上限后拒绝新的身份，而已知身份仍然可以重放。
#include "../test_support.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fake_asr.hpp"
#include "fake_audio.hpp"
#include "fake_rag.hpp"
#include "fake_tts.hpp"
#include "gateway.hpp"
#include "session_app_owner.hpp"
#include "session_runtime.hpp"
#include "supervisor.hpp"

using namespace nexweave;
using nexweave::domain::ErrorCode;
using nexweave::gateway::ConnectionId;
using nexweave::gateway::Gateway;
using nexweave::gateway::GatewayCloseReason;
using nexweave::gateway::GatewayConfig;
using nexweave::gateway::kInvalidConnectionId;
using nexweave::protocol::ControlRequest;
using nexweave::protocol::ControlResponse;
using nexweave::protocol::DataEvent;
using nexweave::protocol::DataEventType;
using nexweave::runtime::ISessionOwner;
using nexweave::runtime::ISessionOwnerFactory;
using nexweave::runtime::ISessionRunSource;
using nexweave::runtime::SessionAppConfig;
using nexweave::runtime::SessionAppInputMode;
using nexweave::runtime::SessionAppOwnerFactory;
using nexweave::runtime::SessionAppRunRecord;
using nexweave::runtime::SessionAppRunResult;
using nexweave::runtime::SessionTurnResult;
using nexweave::runtime::Supervisor;
using nexweave::runtime::SupervisorConfig;
using nexweave::runtime::SupervisorSessionSpec;
using nexweave::runtime::SupervisorState;

namespace {

using Milliseconds = std::chrono::milliseconds;

// 等待上界。夹具本身极快，等满即说明实现有缺陷，而不是机器慢。
constexpr Milliseconds kWaitBound{4000};
// L1 命中夹具：0.90 的分数走直答，整条链路不需要 LLM。
constexpr double kL1Score = 0.90;
constexpr char kHitText[] = "the capital of france is paris";
// 问句必须是命中文本的子串（确定性检索器的匹配方向是“片段文本包含查询”）。
constexpr char kL1Question[] = "the capital of france";

// 闸门：把“会话正在执行”从一个瞬时状态变成用例可以把握的窗口。它只做同步，不拥有资源。
class Gate {
 public:
  void hold() {
    const std::lock_guard<std::mutex> guard(mutex_);
    held_ = true;
  }

  void release() {
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      held_ = false;
    }
    condition_.notify_all();
  }

  // 闸门打开时立即返回；关闭时阻塞到 release()。它就是“推理还在进行”这个事实本身。
  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return !held_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool held_ = false;
};

// 脚本化会话拥有者：生命周期入口的调用顺序由监督器保证，因此除 request_stop() 之外的状态
// 只被工作线程触碰。run() 发布一条按用例给定轮次构造的运行记录，使入口的事件映射可被精确
// 断言，同时让闸门控制“生成中”的窗口。
class ScriptedOwner final : public ISessionOwner {
 public:
  class Factory;

  ScriptedOwner(Factory& factory, const SupervisorSessionSpec& spec);

  domain::OperationResult start() override;
  domain::OperationResult run() override;
  domain::OperationResult cleanup() noexcept override;
  void request_stop() noexcept override;

 private:
  Factory& factory_;
  SupervisorSessionSpec spec_;
};

// 脚本化工厂：既是拥有者工厂，也是运行记录来源，因此可以直接接到入口的接缝上。
class ScriptedOwner::Factory final : public ISessionOwnerFactory, public ISessionRunSource {
 public:
  void hold_run() { run_gate_.hold(); }
  void release_run() { run_gate_.release(); }
  void hold_cleanup() { cleanup_gate_.hold(); }
  void release_cleanup() { cleanup_gate_.release(); }

  void set_turns(std::vector<SessionTurnResult> turns) { turns_ = std::move(turns); }
  void set_run_error(domain::Error error) { run_error_ = std::move(error); }
  // 模拟“本次会话还没有发布记录”：run() 照常收敛，但运行记录来源里仍是上一次会话的内容。
  // 进程内拥有者总会发布记录，但记录来源是一个可替换的接缝（后续的多进程适配器可能异步
  // 发布），因此入口必须自己核对记录归属，而不是假定“最近一条就是这一条”。
  void set_publish_record(bool publish) { publish_record_ = publish; }

  int stop_requests() const noexcept { return stop_requests_.load(); }

  std::shared_ptr<ISessionOwner> create(const SupervisorSessionSpec& spec,
                                        domain::Error& error) override {
    error = domain::Error{};
    return std::make_shared<ScriptedOwner>(*this, spec);
  }

  std::shared_ptr<const SessionAppRunRecord> last_run() const override {
    const std::lock_guard<std::mutex> guard(mutex_);
    return last_run_;
  }

  void publish(const SessionAppRunRecord& record) {
    auto snapshot = std::make_shared<SessionAppRunRecord>(record);
    const std::lock_guard<std::mutex> guard(mutex_);
    last_run_ = std::move(snapshot);
  }

  // 工作线程在使用这些闸门与配置，测试线程在改写它们；用例的时序保证了改写发生在会话开始
  // 之前，因此这里只需要最朴素的同步，不需要额外的可见性协议。
  Gate run_gate_;
  Gate cleanup_gate_;
  std::vector<SessionTurnResult> turns_;
  domain::Error run_error_{};
  bool publish_record_ = true;
  std::atomic<int> stop_requests_{0};

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<const SessionAppRunRecord> last_run_;
};

ScriptedOwner::ScriptedOwner(Factory& factory, const SupervisorSessionSpec& spec)
    : factory_(factory), spec_(spec) {}

domain::OperationResult ScriptedOwner::start() {
  return domain::OperationResult::success();
}

domain::OperationResult ScriptedOwner::run() {
  factory_.run_gate_.wait();
  SessionAppRunResult result;
  result.turns = factory_.turns_;
  for (const auto& turn : result.turns) {
    if (turn.cancelled) {
      ++result.turns_failed;
    } else {
      ++result.turns_completed;
    }
  }
  result.error = factory_.run_error_;
  // 运行记录在 run() 内发布：会话序号由监督器分配并写回 spec，因此记录天然带着归属信息。
  if (factory_.publish_record_) {
    factory_.publish(SessionAppRunRecord{spec_, result});
  }
  return domain::OperationResult{result.error};
}

domain::OperationResult ScriptedOwner::cleanup() noexcept {
  factory_.cleanup_gate_.wait();
  return domain::OperationResult::success();
}

void ScriptedOwner::request_stop() noexcept {
  // 只做一次原子自增，满足“非阻塞、不分配、不抛异常、可重复调用”的接缝契约。
  factory_.stop_requests_.fetch_add(1);
}

// 闸门化夹具：工厂 → 监督器 → 入口 的构造顺序就是借用顺序的逆序，析构时自动反向释放。
struct ScriptedFixture {
  ScriptedOwner::Factory factory;
  Supervisor supervisor;
  Gateway gateway;
  ConnectionId client = kInvalidConnectionId;

  explicit ScriptedFixture(GatewayConfig config = {})
      : supervisor(factory, SupervisorConfig{Milliseconds{4000}, Milliseconds{4000}}),
        gateway(supervisor, factory, config) {
    client = gateway.open_connection();
  }

  // 析构前先把闸门放行并等会话收敛。这不是为了“跑完”，而是为了让断言失败也能**立刻失败**：
  // 会话停在闸门上时拥有者线程正阻塞在夹具自己的条件变量里，而在线程等待期间销毁条件变量
  // 会让销毁方一直等下去——一次断言失败就变成一次挂起。等待是有界的，闸门已经放行，因此
  // 正常路径上这一步立即返回。
  ~ScriptedFixture() {
    factory.release_run();
    factory.release_cleanup();
    supervisor.shutdown(Milliseconds{4000});
  }
};

// 播放逻辑时钟：每次读取前进一个帧时长，使交付的每一帧都立即结算为“已播放”。刻度与读取
// 次数绑定，因此“合成结束后确实播完”在毫秒级预算内即可成立，不引入墙钟或睡眠。
class TickingClock final : public runtime::IPlaybackClock {
 public:
  std::int64_t now_ms() const noexcept override {
    const_cast<TickingClock*>(this)->now_ms_ += domain::kAudioFrameDurationMs;
    return now_ms_;
  }

  void advance(std::int64_t delta_ms) override {
    if (delta_ms > 0) {
      now_ms_ += delta_ms;
    }
  }

 private:
  std::int64_t now_ms_ = 0;
};

// 真实会话夹具：文本模式走 L1 直答，不打开音频源、不需要 LLM。
struct AppFixture {
  backend::FakeRag retriever;
  backend::FakeRagRouter router;
  backend::FakeTts tts;
  backend::FakeAsr asr;
  TickingClock clock;
  backend::FakeAudioSink sink;
  runtime::LogicalClockPlayback playback;

  explicit AppFixture(std::string asr_text = "unrelated fixture text")
      : retriever({capability::RetrievedChunk{"geo-capital-fr", kHitText, kL1Score}}),
        router(retriever, backend::FakeRagRouter::Config{0.90, 0.50, 3}),
        asr(std::vector<std::string>{std::move(asr_text)}),
        playback(sink, clock) {
    // 汇必须先打开：未打开的写入是设备错误，而不是被忽略的调用。
    CHECK(sink.open().ok());
  }
};

SessionAppConfig TextModeConfig() {
  SessionAppConfig config;
  config.mode = SessionAppInputMode::kText;
  config.text = kL1Question;
  config.stream_id = "gateway-app";
  return config;
}

// ---- 收发工具 ----

std::string Frame(const ControlRequest& request) {
  const auto encoded = protocol::encode_request(request);
  CHECK(encoded.ok());
  return *encoded.value + "\n";
}

ControlRequest Request(const std::string& request_id, const std::string& operation,
                       const std::string& work_id = std::string(),
                       const std::string& session_id = std::string(),
                       Milliseconds deadline = Milliseconds{2000}) {
  ControlRequest request;
  request.request_id = request_id;
  request.operation = operation;
  request.work_id = work_id;
  request.session_id = session_id;
  request.deadline = deadline;
  return request;
}

std::string Drain(Gateway& gateway, ConnectionId id) {
  std::string out;
  // 真实传输按“当前可写字节数”分批取；这里一次取完，使用例的注意力留在语义上。
  while (gateway.flush(id, out, 4096) > 0) {
  }
  return out;
}

std::vector<std::string> Lines(const std::string& bytes) {
  std::vector<std::string> lines;
  std::size_t position = 0;
  while (position < bytes.size()) {
    const std::size_t newline = bytes.find('\n', position);
    // 每一条对外消息都必须自成一行：缺换行说明两条消息粘成了不可切分的一行。
    CHECK(newline != std::string::npos);
    lines.push_back(bytes.substr(position, newline - position));
    position = newline + 1;
  }
  return lines;
}

// 用协议层自己的解码器读回入口产生的字节：这样“线上消息合法”这件事由被测代码之外的一层
// 独立确认，而不是由测试自己拼字符串比较。
ControlResponse Response(const std::string& line) {
  const auto decoded = protocol::decode_response(line);
  CHECK(decoded.ok());
  return *decoded.value;
}

DataEvent Event(const std::string& line) {
  const auto decoded = protocol::decode_event_metadata(line);
  CHECK(decoded.ok());
  return *decoded.value;
}

bool IsResponse(const std::string& line) {
  return protocol::decode_response(line).ok();
}

// 从事实行里取一个 key=value。行格式由入口的公共契约固定：空格分隔的 key=value，
// 键顺序固定，值不含空格。
std::string Fact(const std::string& message, const std::string& key) {
  std::size_t position = 0;
  while (position < message.size()) {
    const std::size_t space = message.find(' ', position);
    const std::string token =
        message.substr(position, space == std::string::npos ? std::string::npos : space - position);
    const std::size_t equals = token.find('=');
    if (equals != std::string::npos && token.substr(0, equals) == key) {
      return token.substr(equals + 1);
    }
    if (space == std::string::npos) {
      break;
    }
    position = space + 1;
  }
  return std::string();
}

// 反复交付直到在途会话收敛。交付只做非阻塞判断，因此这里必须有一个显式上界：等满即
// 说明会话没有收敛，而不是“再等等就好了”。
void Settle(Gateway& gateway) {
  const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
  while (gateway.status().session_in_flight) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    gateway.deliver_settled();
    std::this_thread::sleep_for(Milliseconds{1});
  }
}

// 等槽位收敛但**不**交付事件：用来制造“会话已经结束、结果还没有送出去”的窗口，它是
// “新会话不得顶掉上一轮结果”这条不变量的触发条件。返回时断言结果确实还没有交付。
void WaitForSlotSettledWithoutDelivery(Gateway& gateway) {
  const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
  while (gateway.status().supervisor.state != SupervisorState::kIdle) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(Milliseconds{1});
  }
  CHECK(gateway.status().session_in_flight);
}

SessionTurnResult Turn(std::string text) {
  SessionTurnResult turn;
  turn.text = std::move(text);
  turn.completed = true;
  return turn;
}

SessionTurnResult CancelledTurn(std::string text) {
  SessionTurnResult turn;
  turn.text = std::move(text);
  turn.cancelled = true;
  return turn;
}

// ---- 用例 ----

// 保护不变量 1：受理与终态分开。start 的响应先到，事件与会话收敛状态无关地后到，
// 因此客户端不能把“受理成功”读成“这轮已经成功”。
void TestAcceptResponsePrecedesTerminalEvent() {
  ScriptedFixture fixture;
  fixture.factory.hold_run();
  fixture.factory.set_turns({Turn("first answer")});

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  const std::vector<std::string> accepted = Lines(Drain(fixture.gateway, fixture.client));
  CHECK(accepted.size() == 1);
  const ControlResponse response = Response(accepted.front());
  CHECK(response.request_id == "r-1");
  CHECK(response.result.ok());
  CHECK(!response.replayed);
  CHECK(Fact(response.result.error.message, "session_sequence") == "1");
  CHECK(Fact(response.result.error.message, "work_id") == "w-1");
  CHECK(Fact(response.result.error.message, "session_id") == "s-1");

  // 生成仍然停在闸门上：此时不得有任何终态事件，否则“受理”就被当成了“完成”。
  CHECK(fixture.gateway.deliver_settled() == 0);
  CHECK(fixture.supervisor.status().state == SupervisorState::kActive);
  CHECK(fixture.gateway.connection_open(fixture.client));

  fixture.factory.release_run();
  Settle(fixture.gateway);
  const std::vector<std::string> settled = Lines(Drain(fixture.gateway, fixture.client));
  CHECK(settled.size() == 2);
  CHECK(IsResponse(settled.at(0)) == false);

  const DataEvent token = Event(settled.at(0));
  CHECK(token.type == DataEventType::kToken);
  CHECK(token.text == "first answer");
  CHECK(token.request_id == "r-1");
  CHECK(token.session_id == "s-1");
  CHECK(token.sequence == 1);
  CHECK(!token.end);

  const DataEvent terminal = Event(settled.at(1));
  CHECK(terminal.type == DataEventType::kDone);
  CHECK(terminal.end);
  CHECK(terminal.error_code == ErrorCode::kNone);
  CHECK(terminal.request_id == "r-1");
  CHECK(terminal.sequence == 2);

  const auto status = fixture.gateway.status();
  CHECK(status.supervisor.sessions_started == 1);
  CHECK(status.supervisor.sessions_completed == 1);
  CHECK(status.data_events == 2);
  CHECK(status.control_responses == 1);
}

// 保护不变量 2：生成仍在进行时，查询照常被答复，第二次创建得到结构化忙碌且真的没有建立
// 第二个会话——忙碌不是“排队等一会儿”，而是明确的拒绝。
void TestQueryAndBusyStartWhileGenerationIsInFlight() {
  ScriptedFixture fixture;
  fixture.factory.hold_run();
  fixture.factory.set_turns({Turn("held answer")});

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  CHECK(Lines(Drain(fixture.gateway, fixture.client)).size() == 1);

  CHECK(fixture.gateway.feed(fixture.client, Frame(Request("r-2", "query"))));
  const std::vector<std::string> queried = Lines(Drain(fixture.gateway, fixture.client));
  CHECK(queried.size() == 1);
  const ControlResponse query = Response(queried.front());
  CHECK(query.request_id == "r-2");
  CHECK(query.result.ok());
  CHECK(Fact(query.result.error.message, "state") == "active");
  CHECK(Fact(query.result.error.message, "work_id") == "w-1");
  CHECK(Fact(query.result.error.message, "session_sequence") == "1");

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-3", "start", "w-2", "s-2"))));
  const std::vector<std::string> busy = Lines(Drain(fixture.gateway, fixture.client));
  CHECK(busy.size() == 1);
  const ControlResponse rejected = Response(busy.front());
  CHECK(!rejected.result.ok());
  CHECK(rejected.result.error.code == ErrorCode::kBusy);
  // 忙碌拒绝必须没有副作用：被拒绝的那次创建不能留下第二个会话。
  CHECK(fixture.supervisor.status().sessions_started == 1);
  CHECK(fixture.gateway.status().supervisor.sessions_started == 1);
  CHECK(fixture.gateway.status().session_in_flight);

  fixture.factory.release_run();
  Settle(fixture.gateway);
  const std::vector<std::string> settled = Lines(Drain(fixture.gateway, fixture.client));
  CHECK(settled.size() == 2);
  CHECK(Event(settled.back()).type == DataEventType::kDone);
}

// 保护不变量 3：取消响应报告“受理 + 清理快照”，终态另行报告；两者不互相冒充。
void TestCancelReportsAcceptanceSeparatelyFromTerminal() {
  ScriptedFixture fixture;
  fixture.factory.hold_run();
  fixture.factory.set_turns({Turn("interrupted answer")});

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  Drain(fixture.gateway, fixture.client);

  CHECK(fixture.gateway.feed(fixture.client, Frame(Request("r-2", "cancel", "w-1"))));
  const std::vector<std::string> cancelled = Lines(Drain(fixture.gateway, fixture.client));
  CHECK(cancelled.size() == 1);
  const ControlResponse cancel = Response(cancelled.front());
  CHECK(cancel.request_id == "r-2");
  CHECK(cancel.result.ok());
  CHECK(Fact(cancel.result.error.message, "accepted") == "1");
  // 清理还没完成：响应必须如实报告，而不是让“受理了取消”看起来像“设备已经静默”。
  CHECK(Fact(cancel.result.error.message, "cleanup_completed") == "0");
  CHECK(Fact(cancel.result.error.message, "state") == "quiescing");
  CHECK(fixture.supervisor.status().state == SupervisorState::kQuiescing);
  // 取消确实通知到了拥有者，而不是只在监督器里改了状态。
  CHECK(fixture.factory.stop_requests() == 1);

  fixture.factory.release_run();
  Settle(fixture.gateway);
  const std::vector<std::string> settled = Lines(Drain(fixture.gateway, fixture.client));
  CHECK(settled.size() == 2);
  const DataEvent terminal = Event(settled.back());
  CHECK(terminal.type == DataEventType::kError);
  CHECK(terminal.end);
  CHECK(terminal.error_code == ErrorCode::kCancelled);
  CHECK(fixture.supervisor.status().sessions_cancelled == 1);
  CHECK(fixture.supervisor.status().sessions_completed == 0);
}

// 保护不变量 3：取消等待预算耗尽是一个显式的超时，而不是被悄悄当成成功。
void TestCancelWaitBudgetTimeoutIsExplicit() {
  GatewayConfig config;
  config.cancel_wait_budget = Milliseconds{50};
  ScriptedFixture fixture(config);
  fixture.factory.hold_run();
  fixture.factory.set_turns({Turn("slow cleanup")});

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  Drain(fixture.gateway, fixture.client);
  CHECK(fixture.gateway.feed(fixture.client, Frame(Request("r-2", "cancel", "w-1"))));

  const std::vector<std::string> lines = Lines(Drain(fixture.gateway, fixture.client));
  CHECK(lines.size() == 1);
  const ControlResponse cancel = Response(lines.front());
  CHECK(!cancel.result.ok());
  CHECK(cancel.result.error.code == ErrorCode::kTimeout);
  // 超时并不改变“取消已被受理”这个事实，两个事实必须同时可读。
  CHECK(Fact(cancel.result.error.message, "accepted") == "1");
  CHECK(Fact(cancel.result.error.message, "cleanup_completed") == "0");
  CHECK(fixture.supervisor.status().state == SupervisorState::kQuiescing);

  fixture.factory.release_run();
  Settle(fixture.gateway);
  const std::vector<std::string> settled = Lines(Drain(fixture.gateway, fixture.client));
  CHECK(Event(settled.back()).error_code == ErrorCode::kCancelled);
}

// 保护不变量 3：会话已经自然收尾时取消不假装受理，但仍然如实报告清理已完成。
void TestCancelOnFinishedSessionReportsNoAcceptance() {
  ScriptedFixture fixture;
  fixture.factory.set_turns({Turn("done answer")});

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  Drain(fixture.gateway, fixture.client);
  Settle(fixture.gateway);
  CHECK(Lines(Drain(fixture.gateway, fixture.client)).size() == 2);

  CHECK(fixture.gateway.feed(fixture.client, Frame(Request("r-2", "cancel", "w-1"))));
  const std::vector<std::string> lines = Lines(Drain(fixture.gateway, fixture.client));
  CHECK(lines.size() == 1);
  const ControlResponse cancel = Response(lines.front());
  CHECK(cancel.result.ok());
  CHECK(Fact(cancel.result.error.message, "accepted") == "0");
  CHECK(Fact(cancel.result.error.message, "cleanup_completed") == "1");
  CHECK(Fact(cancel.result.error.message, "state") == "idle");
}

// 保护不变量 4：同一请求无论被切成多少片，结果都一样；一个批次里的多个帧各自成答。
void TestSplitAndStickyInputsAreEquivalent() {
  const std::string frame = Frame(Request("r-1", "query"));

  ScriptedFixture whole;
  CHECK(whole.gateway.feed(whole.client, frame));
  const std::string whole_bytes = Drain(whole.gateway, whole.client);

  ScriptedFixture split;
  for (std::size_t index = 0; index + 1 < frame.size(); ++index) {
    // 半包阶段不产生任何输出：残缺 JSON 不该被解析，更不该被答复。
    CHECK(split.gateway.feed(split.client, std::string(1, frame.at(index))));
    CHECK(Drain(split.gateway, split.client).empty());
  }
  CHECK(split.gateway.feed(split.client, std::string(1, frame.back())));
  CHECK(Drain(split.gateway, split.client) == whole_bytes);

  // 粘包：两帧一个批次，得到两条按顺序排列的响应。
  ScriptedFixture sticky;
  CHECK(sticky.gateway.feed(sticky.client,
                            Frame(Request("r-1", "query")) + Frame(Request("r-2", "query"))));
  const std::vector<std::string> lines = Lines(Drain(sticky.gateway, sticky.client));
  CHECK(lines.size() == 2);
  CHECK(Response(lines.at(0)).request_id == "r-1");
  CHECK(Response(lines.at(1)).request_id == "r-2");
}

// 保护不变量 5：重复 request_id 不启动第二个任务；执行中、已完成、字段冲突三种情形分别有
// 明确结果。
void TestDuplicateRequestIdDoesNotStartTwice() {
  ScriptedFixture fixture;
  fixture.factory.hold_run();
  fixture.factory.set_turns({Turn("once only")});

  const ControlRequest start = Request("r-1", "start", "w-1", "s-1");
  CHECK(fixture.gateway.feed(fixture.client, Frame(start)));
  const ControlResponse first = Response(Lines(Drain(fixture.gateway, fixture.client)).front());
  CHECK(first.result.ok());

  // 执行中重复：返回 kAlreadyCompleted，不启动第二次操作。
  CHECK(fixture.gateway.feed(fixture.client, Frame(start)));
  const ControlResponse running = Response(Lines(Drain(fixture.gateway, fixture.client)).front());
  CHECK(!running.result.ok());
  CHECK(running.result.error.code == ErrorCode::kAlreadyCompleted);
  CHECK(fixture.supervisor.status().sessions_started == 1);

  // 字段冲突：同一个 request_id 换了 work_id，属于非法重试而不是重放。
  CHECK(fixture.gateway.feed(fixture.client, Frame(Request("r-1", "start", "w-9", "s-1"))));
  const ControlResponse conflict = Response(Lines(Drain(fixture.gateway, fixture.client)).front());
  CHECK(!conflict.result.ok());
  CHECK(conflict.result.error.code == ErrorCode::kInvalidInput);
  // 非法重试同样不得启动任务。
  CHECK(fixture.supervisor.status().sessions_started == 1);

  fixture.factory.release_run();
  Settle(fixture.gateway);
  Drain(fixture.gateway, fixture.client);

  // 已完成重复：重放原受理响应并标记 replayed，因此客户端能区分“重放”与“新执行”。
  CHECK(fixture.gateway.feed(fixture.client, Frame(start)));
  const ControlResponse replayed = Response(Lines(Drain(fixture.gateway, fixture.client)).front());
  CHECK(replayed.result.ok());
  CHECK(replayed.replayed);
  CHECK(replayed.result.error.message == first.result.error.message);
  CHECK(fixture.supervisor.status().sessions_started == 1);
  CHECK(fixture.supervisor.status().sessions_completed == 1);
  CHECK(fixture.gateway.status().requests_replayed == 1);
}

// 保护不变量 6：无法归属的输入关闭连接；能归属的非法输入回结构化错误并保留连接。
void TestUnaddressableInputClosesAndAddressableInputSurvives() {
  ScriptedFixture fixture;
  CHECK(!fixture.gateway.feed(fixture.client, "this is not json\n"));
  const auto stats = fixture.gateway.connection_stats(fixture.client);
  CHECK(stats.known);
  CHECK(!stats.open);
  // v1 不允许没有 request_id 的响应，因此这里没有可寻址的答复对象：记录原因并关闭。
  CHECK(stats.close_reason == GatewayCloseReason::kProtocolViolation);
  CHECK(Drain(fixture.gateway, fixture.client).empty());
  CHECK(fixture.gateway.status().malformed_frames == 1);

  // 缺失 request_id 的帧同样无法归属。
  const ConnectionId second = fixture.gateway.open_connection();
  CHECK(second != kInvalidConnectionId);
  CHECK(!fixture.gateway.feed(second, "{\"version\":1,\"operation\":\"query\"}\n"));
  CHECK(fixture.gateway.connection_stats(second).close_reason ==
        GatewayCloseReason::kProtocolViolation);

  // 有 request_id 但字段/取值非法的帧可以归属：回结构化错误，连接保持可用。
  const ConnectionId third = fixture.gateway.open_connection();
  CHECK(third != kInvalidConnectionId);
  const std::string unknown_operation =
      "{\"version\":1,\"request_id\":\"r-9\",\"operation\":\"teleport\",\"work_id\":\"\","
      "\"session_id\":\"\",\"generation\":0,\"deadline_ms\":1000}\n";
  CHECK(fixture.gateway.feed(third, unknown_operation));
  const std::vector<std::string> rejected = Lines(Drain(fixture.gateway, third));
  CHECK(rejected.size() == 1);
  const ControlResponse response = Response(rejected.front());
  CHECK(response.request_id == "r-9");
  CHECK(!response.result.ok());
  CHECK(response.result.error.code == ErrorCode::kInvalidInput);

  // 错误之后连接仍可用：下一条合法请求照常被处理。
  CHECK(fixture.gateway.feed(third, Frame(Request("r-10", "query"))));
  const std::vector<std::string> after = Lines(Drain(fixture.gateway, third));
  CHECK(after.size() == 1);
  CHECK(Response(after.front()).request_id == "r-10");
  CHECK(Response(after.front()).result.ok());
}

// 保护不变量 7：单帧超长导致关闭，且与该超长帧同批到达的输入整体作废——本入口不保证批内
// 先后关系，因此不先执行随后的请求再退出。
void TestOversizedFrameClosesAndDiscardsTheBatch() {
  GatewayConfig config;
  config.max_frame_bytes = 160;
  ScriptedFixture fixture(config);

  const std::string batch = Frame(Request("r-1", "query")) + std::string(400, 'x') + "\n";
  CHECK(!fixture.gateway.feed(fixture.client, batch));
  const auto stats = fixture.gateway.connection_stats(fixture.client);
  CHECK(!stats.open);
  CHECK(stats.close_reason == GatewayCloseReason::kOversizedFrame);
  CHECK(stats.frames_received == 0);
  CHECK(Drain(fixture.gateway, fixture.client).empty());
  CHECK(fixture.gateway.status().oversized_frames == 1);

  // 超长帧之后重新连上仍然是可用的：拒绝的是那一次输入，不是整个入口。
  const ConnectionId next = fixture.gateway.open_connection();
  CHECK(next != kInvalidConnectionId);
  CHECK(fixture.gateway.feed(next, Frame(Request("r-2", "query"))));
  CHECK(Response(Lines(Drain(fixture.gateway, next)).front()).result.ok());
}

// 保护不变量 7：慢客户端在待发缓冲达到声明上限时被关闭，缓冲不会继续增长。
void TestSlowClientClosesAtTheDeclaredBound() {
  GatewayConfig config;
  // 上限按“装得下一条事件、装不下第二条”取值：这样用例验证的是**累积**导致的关闭，
  // 而不是“单条事件天生放不下”。真实慢客户端的表现正是前者。
  config.max_pending_data_bytes = 260;
  ScriptedFixture fixture(config);

  std::vector<SessionTurnResult> turns;
  for (int index = 0; index < 8; ++index) {
    turns.push_back(Turn("answer number " + std::to_string(index) + " with trailing text"));
  }
  fixture.factory.set_turns(std::move(turns));

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  Drain(fixture.gateway, fixture.client);
  Settle(fixture.gateway);

  const auto stats = fixture.gateway.connection_stats(fixture.client);
  CHECK(!stats.open);
  CHECK(stats.close_reason == GatewayCloseReason::kSlowClient);
  // 关键不变量：待发数据始终不超过声明上限，因此“客户端不读”不会变成无限内存占用。
  CHECK(stats.pending_data_bytes <= config.max_pending_data_bytes);
  CHECK(fixture.gateway.status().slow_client_closes == 1);
  // 被关闭的连接仍然可以被取走已产生的字节，客户端因此能看到一段有界的、不掺假的输出。
  CHECK(!Drain(fixture.gateway, fixture.client).empty());
}

// 保护不变量 7 的另一面：只要客户端及时取走字节，同一场会话不会被误判为慢客户端。
void TestTimelyFlushKeepsConnectionHealthy() {
  ScriptedFixture fixture;
  std::vector<SessionTurnResult> turns;
  for (int index = 0; index < 8; ++index) {
    turns.push_back(Turn("answer number " + std::to_string(index) + " with trailing text"));
  }
  fixture.factory.set_turns(std::move(turns));

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  // 先取走受理响应：它与数据事件分属两条队列，混在一起收集会让“第几条是什么”失去意义。
  const ControlResponse accepted =
      Response(Lines(Drain(fixture.gateway, fixture.client)).front());
  CHECK(accepted.result.ok());

  const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
  std::vector<std::string> lines;
  while (lines.size() < 9) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    fixture.gateway.deliver_settled();
    for (auto& line : Lines(Drain(fixture.gateway, fixture.client))) {
      lines.push_back(std::move(line));
    }
    std::this_thread::sleep_for(Milliseconds{1});
  }
  CHECK(fixture.gateway.connection_open(fixture.client));
  CHECK(Event(lines.front()).type == DataEventType::kToken);
  CHECK(Event(lines.back()).type == DataEventType::kDone);
}

// 保护不变量 4 与 1：退出超时不等于退出失败，重复调用可以继续等；退出响应在连接关闭之后
// 仍然可以被取走。
void TestExitIsIdempotentAndItsResponseSurvivesClose() {
  GatewayConfig config;
  config.exit_wait_budget = Milliseconds{50};
  ScriptedFixture fixture(config);
  fixture.factory.hold_run();
  fixture.factory.set_turns({Turn("long answer")});

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  Drain(fixture.gateway, fixture.client);

  // 会话仍停在闸门上：退出超时，运行时没有退出，因此入口保持可用以便重试。
  CHECK(fixture.gateway.feed(fixture.client, Frame(Request("r-2", "exit"))));
  const ControlResponse timeout = Response(Lines(Drain(fixture.gateway, fixture.client)).front());
  CHECK(!timeout.result.ok());
  CHECK(timeout.result.error.code == ErrorCode::kTimeout);
  CHECK(Fact(timeout.result.error.message, "accepted") == "1");
  CHECK(Fact(timeout.result.error.message, "state") == "quiescing");
  CHECK(!fixture.gateway.status().closed);
  CHECK(fixture.gateway.connection_open(fixture.client));

  fixture.factory.release_run();
  Settle(fixture.gateway);
  const std::vector<std::string> settled = Lines(Drain(fixture.gateway, fixture.client));
  // 退出受理过一次停止，因此这次会话以取消终态收敛，而不是被报告成“正常完成”；
  // 已经交付过的文本仍然照常给出，它确实产生过。
  CHECK(settled.size() == 2);
  CHECK(Event(settled.front()).type == DataEventType::kToken);
  CHECK(Event(settled.back()).error_code == ErrorCode::kCancelled);
  CHECK(fixture.supervisor.status().sessions_cancelled == 1);

  // 退出会关闭全部连接，因此这一帧的返回值为假；但退出响应不会被关闭吞掉，仍可被取走。
  CHECK(!fixture.gateway.feed(fixture.client, Frame(Request("r-3", "exit"))));
  const std::vector<std::string> lines = Lines(Drain(fixture.gateway, fixture.client));
  CHECK(lines.size() == 1);
  const ControlResponse exited = Response(lines.front());
  CHECK(exited.result.ok());
  CHECK(Fact(exited.result.error.message, "accepted") == "1");
  CHECK(Fact(exited.result.error.message, "state") == "closed");
  CHECK(fixture.gateway.status().closed);
  CHECK(!fixture.gateway.connection_open(fixture.client));
  // 关闭不再接受输入，但已经产生的响应不会被关闭吞掉。
  CHECK(!fixture.gateway.feed(fixture.client, Frame(Request("r-4", "query"))));
  // 两次退出请求都被处理过：第一次超时，第二次才真正退出。计数与“退没退”是两件事。
  CHECK(fixture.gateway.status().exit_requests == 2);
  // 运行时已经退出：新的连接不再被接受，而不是接受后立刻失败。
  CHECK(fixture.gateway.open_connection() == kInvalidConnectionId);
}

// 保护不变量 8：只有发起会话的那条连接断开才受理取消；断连后的终态没有接收者，被记账。
void TestDisconnectCancelsOnlyTheOwningConnectionsSession() {
  ScriptedFixture fixture;
  fixture.factory.hold_run();
  fixture.factory.set_turns({Turn("abandoned answer")});

  const ConnectionId other = fixture.gateway.open_connection();
  CHECK(other != kInvalidConnectionId);
  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  Drain(fixture.gateway, fixture.client);

  // 另一条连接可以查询同一台设备的会话，但它的断开与这次会话无关。
  CHECK(fixture.gateway.feed(other, Frame(Request("r-2", "query"))));
  CHECK(Response(Lines(Drain(fixture.gateway, other)).front()).result.ok());
  fixture.gateway.close_connection(other);
  CHECK(!fixture.gateway.connection_open(other));
  CHECK(!fixture.supervisor.status().cancel_accepted);
  CHECK(fixture.supervisor.status().state == SupervisorState::kActive);
  CHECK(fixture.gateway.status().disconnects_cancelling_session == 0);

  // 发起会话的连接断开：输出已经没有接收者，因此受理取消。
  fixture.gateway.close_connection(fixture.client);
  CHECK(fixture.supervisor.status().cancel_accepted);
  CHECK(fixture.supervisor.status().state == SupervisorState::kQuiescing);
  CHECK(fixture.gateway.status().disconnects_cancelling_session == 1);

  fixture.factory.release_run();
  Settle(fixture.gateway);
  CHECK(fixture.supervisor.status().sessions_cancelled == 1);
  // 终态确实产生了，只是没有接收者：它被记入丢弃账目，而不是静默消失。
  CHECK(fixture.gateway.status().events_dropped_without_receiver >= 1);
  CHECK(fixture.gateway.status().data_events == 0);
}

// 保护不变量 9：会话事件按会话序号核对，因此上一次会话的记录不会漏进下一次。
void TestSessionEventsAreMatchedBySequence() {
  ScriptedFixture fixture;
  fixture.factory.set_turns({Turn("first session answer")});

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  Drain(fixture.gateway, fixture.client);
  Settle(fixture.gateway);
  CHECK(Lines(Drain(fixture.gateway, fixture.client)).size() == 2);

  // 第二次会话停在闸门上。此时运行记录里仍然是第一次会话的结果，入口必须一条事件都不发。
  fixture.factory.hold_run();
  fixture.factory.set_turns({Turn("second session answer")});
  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-2", "start", "w-2", "s-2"))));
  Drain(fixture.gateway, fixture.client);
  CHECK(fixture.gateway.deliver_settled() == 0);
  CHECK(Drain(fixture.gateway, fixture.client).empty());

  fixture.factory.release_run();
  Settle(fixture.gateway);
  const std::vector<std::string> settled = Lines(Drain(fixture.gateway, fixture.client));
  CHECK(settled.size() == 2);
  const DataEvent token = Event(settled.front());
  CHECK(token.text == "second session answer");
  CHECK(token.session_id == "s-2");
  CHECK(token.request_id == "r-2");
  CHECK(Event(settled.back()).type == DataEventType::kDone);
}

// 保护不变量 9：会话收敛但记录来源还没有给出本次会话的记录时，入口一条事件都不发，
// 而不是把上一次会话的文本当成本次输出。
void TestStaleRunRecordIsNotDeliveredAsCurrentSession() {
  ScriptedFixture fixture;
  fixture.factory.set_turns({Turn("first session answer")});

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  Drain(fixture.gateway, fixture.client);
  Settle(fixture.gateway);
  CHECK(Lines(Drain(fixture.gateway, fixture.client)).size() == 2);

  // 第二次会话不再发布记录：来源里留着的仍然是第一次会话的文本。
  fixture.factory.set_publish_record(false);
  fixture.factory.set_turns({Turn("never delivered")});
  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-2", "start", "w-2", "s-2"))));
  Drain(fixture.gateway, fixture.client);
  Settle(fixture.gateway);

  const std::vector<std::string> settled = Lines(Drain(fixture.gateway, fixture.client));
  // 只有终态：上一次会话的文本一条都不许出现。序号也从本次会话重新开始，不延续上一次。
  CHECK(settled.size() == 1);
  const DataEvent terminal = Event(settled.front());
  CHECK(terminal.type == DataEventType::kDone);
  CHECK(terminal.end);
  CHECK(terminal.request_id == "r-2");
  CHECK(terminal.session_id == "s-2");
  CHECK(terminal.sequence == 1);
  CHECK(terminal.text.empty());
  CHECK(fixture.supervisor.status().sessions_completed == 2);
}

// 保护不变量 11：请求自带的处理预算收紧本入口自己的等待，配置里的上界不能反过来盖过它。
void TestRequestDeadlineClampsTheCancelWaitBudget() {
  GatewayConfig config;
  // 配置上界刻意远大于请求自带的预算：真正生效的必须是两者中较小的那个。
  config.cancel_wait_budget = Milliseconds{5000};
  ScriptedFixture fixture(config);
  fixture.factory.hold_run();
  fixture.factory.set_turns({Turn("held answer")});

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  Drain(fixture.gateway, fixture.client);

  const auto begin = std::chrono::steady_clock::now();
  CHECK(fixture.gateway.feed(
      fixture.client,
      Frame(Request("r-2", "cancel", "w-1", std::string(), Milliseconds{60}))));
  const auto elapsed = std::chrono::steady_clock::now() - begin;

  const ControlResponse cancel = Response(Lines(Drain(fixture.gateway, fixture.client)).front());
  CHECK(!cancel.result.ok());
  CHECK(cancel.result.error.code == ErrorCode::kTimeout);
  // 会话停在闸门上，因此清理不可能在预算内完成；关键是这次等待被请求预算收紧，而不是等满
  // 配置里的 5 秒。上界留了宽松余量，避免把机器慢误判成没生效。
  CHECK(elapsed < Milliseconds{2000});
  CHECK(Fact(cancel.result.error.message, "accepted") == "1");
  CHECK(Fact(cancel.result.error.message, "cleanup_completed") == "0");

  fixture.factory.release_run();
  Settle(fixture.gateway);
  Drain(fixture.gateway, fixture.client);
}

// 保护不变量 10：上一次会话已经收敛但结果还没交付时，新的创建必须先把它交付掉。
void TestSettledSessionIsDeliveredBeforeTheNextStart() {
  ScriptedFixture fixture;
  fixture.factory.set_turns({Turn("first session answer")});

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  Drain(fixture.gateway, fixture.client);
  WaitForSlotSettledWithoutDelivery(fixture.gateway);

  // 第二次创建发生在“槽位空闲、结果未交付”的窗口里；第二次会话停在闸门上，使交付顺序不被
  // 它的收敛打扰。
  fixture.factory.hold_run();
  fixture.factory.set_turns({Turn("second session answer")});
  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-2", "start", "w-2", "s-2"))));

  // 控制响应与数据事件分属两条队列，跨队列先后不属于契约，因此这里只核对内容与归属：
  // 第二次的受理响应在，第一次的轮次事件与终态也在。
  bool saw_second_accepted = false;
  bool saw_first_token = false;
  bool saw_first_terminal = false;
  for (const auto& line : Lines(Drain(fixture.gateway, fixture.client))) {
    if (IsResponse(line)) {
      const ControlResponse response = Response(line);
      CHECK(response.request_id == "r-2");
      CHECK(response.result.ok());
      saw_second_accepted = true;
      continue;
    }
    const DataEvent event = Event(line);
    // 这一批里不允许出现第二次会话的事件：它还停在闸门上，一条都还没产生。
    CHECK(event.request_id == "r-1");
    CHECK(event.session_id == "s-1");
    if (event.type == DataEventType::kToken) {
      CHECK(event.text == "first session answer");
      saw_first_token = true;
    } else if (event.type == DataEventType::kDone) {
      saw_first_terminal = true;
    }
  }
  CHECK(saw_second_accepted);
  CHECK(saw_first_token);
  CHECK(saw_first_terminal);

  // 第一次的创建已经定局：它的重试重放受理响应，而不是永远停在“执行中”。
  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  const ControlResponse replayed = Response(Lines(Drain(fixture.gateway, fixture.client)).front());
  CHECK(replayed.result.ok());
  CHECK(replayed.replayed);
  CHECK(fixture.gateway.status().requests_replayed == 1);
  CHECK(fixture.supervisor.status().sessions_started == 2);

  fixture.factory.release_run();
}

// 保护不变量 8：服务端先关闭连接之后，传输层报来的对端断开同样要受理取消。
void TestDisconnectAfterServerSideCloseStillCancelsTheSession() {
  GatewayConfig config;
  config.max_frame_bytes = 160;
  ScriptedFixture fixture(config);
  fixture.factory.hold_run();
  fixture.factory.set_turns({Turn("orphan answer")});

  CHECK(fixture.gateway.feed(fixture.client,
                             Frame(Request("r-1", "start", "w-1", "s-1"))));
  Drain(fixture.gateway, fixture.client);
  CHECK(fixture.gateway.status().session_in_flight);

  // 服务端先关闭连接（这里用超长帧触发）：此时会话仍在执行，输出已经没有人读。
  CHECK(!fixture.gateway.feed(fixture.client, std::string(400, 'x') + "\n"));
  CHECK(fixture.gateway.connection_stats(fixture.client).close_reason ==
        GatewayCloseReason::kOversizedFrame);

  // 传输层随后报来对端断开：断连策略必须照样生效，否则这次会话会带着一个永远没人读取的
  // 输出继续占用设备唯一的槽位。
  fixture.gateway.close_connection(fixture.client);
  // 关闭原因保留第一个：后来的对端断开不覆盖“为什么关的”。
  CHECK(fixture.gateway.connection_stats(fixture.client).close_reason ==
        GatewayCloseReason::kOversizedFrame);
  CHECK(fixture.supervisor.status().cancel_accepted);
  CHECK(fixture.gateway.status().disconnects_cancelling_session == 1);

  fixture.factory.release_run();
  Settle(fixture.gateway);
  CHECK(fixture.supervisor.status().sessions_cancelled == 1);
  // 终态确实产生了，只是没有接收者。
  CHECK(fixture.gateway.status().events_dropped_without_receiver >= 1);
}

// 保护不变量 12：幂等记录容量是硬上限——新身份被拒绝，已知身份仍然可以重放。
void TestIdempotencyCapacityRejectsNewIdsButReplaysKnownOnes() {
  GatewayConfig config;
  config.max_idempotency_records = 2;
  ScriptedFixture fixture(config);

  // 两条查询把记录表占满。
  CHECK(fixture.gateway.feed(fixture.client, Frame(Request("r-1", "query"))));
  CHECK(fixture.gateway.feed(fixture.client, Frame(Request("r-2", "query"))));
  CHECK(Lines(Drain(fixture.gateway, fixture.client)).size() == 2);

  // 新身份被拒绝：宁可不做，也不接受一次无法保证“重试不重复执行”的操作。
  CHECK(fixture.gateway.feed(fixture.client, Frame(Request("r-3", "query"))));
  const ControlResponse refused = Response(Lines(Drain(fixture.gateway, fixture.client)).front());
  CHECK(!refused.result.ok());
  CHECK(refused.result.error.code == ErrorCode::kBackendFailure);
  CHECK(fixture.gateway.status().idempotency_rejections == 1);

  // 已知身份仍然可以重放：容量满不该把已经受理过的请求一并锁死。
  CHECK(fixture.gateway.feed(fixture.client, Frame(Request("r-1", "query"))));
  const ControlResponse replayed = Response(Lines(Drain(fixture.gateway, fixture.client)).front());
  CHECK(replayed.result.ok());
  CHECK(replayed.replayed);
  CHECK(fixture.gateway.status().requests_replayed == 1);
  // 查询不创建会话：容量上限不会改变“没有启动任何会话”这一事实。
  CHECK(fixture.supervisor.status().sessions_started == 0);
}

// 保护不变量 7：流在半帧上结束（没有换行）时，那些字节不构成请求，也不静默消失。
void TestPartialFrameIsAccountedWhenTheStreamEnds() {
  ScriptedFixture fixture;
  const std::string partial = "{\"version\":1,\"request";
  CHECK(fixture.gateway.feed(fixture.client, partial));
  // 半包既不是帧，也不会被当成帧执行。
  CHECK(fixture.gateway.connection_stats(fixture.client).frames_received == 0);
  CHECK(Drain(fixture.gateway, fixture.client).empty());

  fixture.gateway.close_connection(fixture.client);
  const auto stats = fixture.gateway.connection_stats(fixture.client);
  CHECK(!stats.open);
  CHECK(stats.close_reason == GatewayCloseReason::kPeerClosed);
  // 丢弃的字节被记账，因此“输入断在半帧上”是可审计的事实。
  CHECK(stats.discarded_partial_bytes == partial.size());
  CHECK(fixture.gateway.status().discarded_partial_bytes == partial.size());
  // 半包不是非法输入：没有可归属的请求要拒绝，也没有帧被解出来。
  CHECK(fixture.gateway.status().malformed_frames == 0);
  CHECK(fixture.gateway.status().control_responses == 0);
}

// 保护不变量 9：同一场景重复执行得到逐字节一致的输出。
void TestRepeatedRunsProduceIdenticalBytes() {
  const auto run = []() {
    ScriptedFixture fixture;
    fixture.factory.set_turns({Turn("deterministic answer"), CancelledTurn("stopped answer")});
    CHECK(fixture.gateway.feed(fixture.client,
                               Frame(Request("r-1", "start", "w-1", "s-1"))));
    Settle(fixture.gateway);
    return Drain(fixture.gateway, fixture.client);
  };
  const std::string first = run();
  const std::string second = run();
  CHECK(!first.empty());
  CHECK(first == second);
}

// 保护不变量 6 与 4：空输入与空行是惰性的，不会产生消息，也不会损坏连接状态。
void TestEmptyInputAndBlankLinesAreInert() {
  ScriptedFixture fixture;
  CHECK(fixture.gateway.feed(fixture.client, ""));
  CHECK(fixture.gateway.feed(fixture.client, "\n\n\r\n"));
  CHECK(Drain(fixture.gateway, fixture.client).empty());
  CHECK(fixture.gateway.connection_open(fixture.client));
  CHECK(fixture.gateway.connection_stats(fixture.client).frames_received == 0);

  CHECK(fixture.gateway.feed(fixture.client, Frame(Request("r-1", "query"))));
  CHECK(Response(Lines(Drain(fixture.gateway, fixture.client)).front()).result.ok());
}

// 保护不变量 1：把真实的会话应用接到入口上，事件映射在真实会话上同样成立。
void TestRealSessionAppDeliversTurnEventsAndSuccessTerminal() {
  AppFixture app;
  SessionAppOwnerFactory owner_factory(TextModeConfig(), app.asr, app.retriever, app.router,
                                      app.tts, app.playback, nullptr, nullptr);
  Supervisor supervisor(owner_factory, SupervisorConfig{Milliseconds{4000}, Milliseconds{4000}});
  Gateway gateway(supervisor, owner_factory);
  const ConnectionId client = gateway.open_connection();
  CHECK(client != kInvalidConnectionId);

  CHECK(gateway.feed(client, Frame(Request("r-1", "start", "w-1", "s-1"))));
  const ControlResponse accepted = Response(Lines(Drain(gateway, client)).front());
  CHECK(accepted.result.ok());

  const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
  std::vector<std::string> lines;
  while (lines.empty() || Event(lines.back()).type != DataEventType::kDone) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    gateway.deliver_settled();
    for (auto& line : Lines(Drain(gateway, client))) {
      lines.push_back(std::move(line));
    }
    std::this_thread::sleep_for(Milliseconds{1});
  }

  // 一轮 L1 直答：一条携带直答文本的 token 事件 + 一条成功终态。
  CHECK(lines.size() == 2);
  const DataEvent token = Event(lines.front());
  CHECK(token.type == DataEventType::kToken);
  CHECK(token.text == kHitText);
  CHECK(token.request_id == "r-1");
  CHECK(token.session_id == "s-1");
  CHECK(!token.end);

  const DataEvent terminal = Event(lines.back());
  CHECK(terminal.type == DataEventType::kDone);
  CHECK(terminal.end);
  CHECK(terminal.error_code == ErrorCode::kNone);
  CHECK(supervisor.status().sessions_completed == 1);
  CHECK(supervisor.status().sessions_started == 1);
  CHECK(gateway.status().data_events == 2);
}

}  // namespace

int main() {
  try {
    TestAcceptResponsePrecedesTerminalEvent();
    TestQueryAndBusyStartWhileGenerationIsInFlight();
    TestCancelReportsAcceptanceSeparatelyFromTerminal();
    TestCancelWaitBudgetTimeoutIsExplicit();
    TestCancelOnFinishedSessionReportsNoAcceptance();
    TestRequestDeadlineClampsTheCancelWaitBudget();
    TestSplitAndStickyInputsAreEquivalent();
    TestDuplicateRequestIdDoesNotStartTwice();
    TestIdempotencyCapacityRejectsNewIdsButReplaysKnownOnes();
    TestUnaddressableInputClosesAndAddressableInputSurvives();
    TestOversizedFrameClosesAndDiscardsTheBatch();
    TestSlowClientClosesAtTheDeclaredBound();
    TestTimelyFlushKeepsConnectionHealthy();
    TestExitIsIdempotentAndItsResponseSurvivesClose();
    TestDisconnectCancelsOnlyTheOwningConnectionsSession();
    TestDisconnectAfterServerSideCloseStillCancelsTheSession();
    TestSessionEventsAreMatchedBySequence();
    TestStaleRunRecordIsNotDeliveredAsCurrentSession();
    TestSettledSessionIsDeliveredBeforeTheNextStart();
    TestRepeatedRunsProduceIdenticalBytes();
    TestEmptyInputAndBlankLinesAreInert();
    TestPartialFrameIsAccountedWhenTheStreamEnds();
    TestRealSessionAppDeliversTurnEventsAndSuccessTerminal();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Gateway 请求入口用例未通过: %s\n", error.what());
    return 1;
  }
  return 0;
}

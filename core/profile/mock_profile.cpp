// Mock profile 确定性入口的实现。职责与对外不变量见 mock_profile.hpp；本文件只说明“为什么
// 这样组织”，不重复头部注释。
//
// 组装顺序与所有权
// ----------------
// 对象按「能力 → 组装 → 监督 → 入口」的依赖方向自下而上建立，析构顺序与之相反：
//   1. MockProfileFixture 拥有全部能力夹具与播放组件；
//   2. SessionAppOwnerFactory 借用它们，为每次会话产出全新的 SessionApp；
//   3. Supervisor 借用工厂（只在 start() 的调用线程上使用它）；
//   4. Gateway 借用监督器与运行记录来源（工厂同时扮演后者）。
// 成员声明顺序把这些约束固定成构造性质，而不是靠调用方记住一条口头规则。
//
// 为什么夹具在内部而不是由调用方注入
// ----------------------------------
// 本层的验收对象是“这条命令的行为”，不是“能不能换后端”。夹具留在内部，调用方就无法通过
// 注入行为不同的后端让场景悄悄改变含义；换后端的验证属于能力契约测试，不属于这里。
//
// 为什么等待都是有界轮询而不是睡眠
// --------------------------------
// 会话在监督器的工作线程上推进，本层只能在“它有没有收敛”这个事实上等待。所有等待都写成
// 「先判断、再让出、直到预算用尽」，预算用尽即按运行级失败报告：把预算写大可以让慢机器也
// 过，但那样“等不到”这件事就再也不会被发现。
#include "mock_profile.hpp"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../backend/fake_asr.hpp"
#include "../backend/fake_audio.hpp"
#include "../backend/fake_llm.hpp"
#include "../backend/fake_rag.hpp"
#include "../backend/fake_tts.hpp"
#include "../domain/error.hpp"
#include "../gateway/gateway.hpp"
#include "../observability/observability.hpp"
#include "../protocol/control_rpc.hpp"
#include "../protocol/data_event.hpp"
#include "../runtime/session_runtime.hpp"
#include "../runtime/supervisor.hpp"
#include "session_app_owner.hpp"

namespace nexweave::app {
namespace {

using nexweave::backend::FakeAsr;
using nexweave::backend::FakeAudioSink;
using nexweave::backend::FakeLlm;
using nexweave::backend::FakeRag;
using nexweave::backend::FakeRagRouter;
using nexweave::backend::FakeTts;
using nexweave::capability::RetrievedChunk;
using nexweave::domain::Error;
using nexweave::domain::ErrorCode;
using nexweave::domain::OperationResult;
using nexweave::gateway::ConnectionId;
using nexweave::gateway::Gateway;
using nexweave::gateway::GatewayCloseReason;
using nexweave::gateway::GatewayConfig;
using nexweave::gateway::kInvalidConnectionId;
using nexweave::protocol::ControlRequest;
using nexweave::protocol::ControlResponse;
using nexweave::protocol::DataEventType;
using nexweave::runtime::LogicalClockPlayback;
using nexweave::runtime::ManualPlaybackClock;
using nexweave::runtime::SessionAppConfig;
using nexweave::runtime::SessionAppInputMode;
using nexweave::runtime::SessionAppOwnerFactory;
using nexweave::runtime::Supervisor;
using nexweave::runtime::SupervisorConfig;
using nexweave::runtime::SupervisorSessionSpec;
using nexweave::runtime::SupervisorState;

using Milliseconds = std::chrono::milliseconds;

// 一次 flush 愿意接收的最大字节数。它只是“调用方一次愿意接收多少”的上界，不是缓冲区大小：
// 真正决定取走多少的是场景配置的取字节预算。
constexpr std::size_t kFlushChunk = 64U * 1024U;
// 等待取消被受理的默认预算。它只覆盖“提交请求 → 监督器受理”这一小步，不覆盖会话执行。
constexpr std::size_t kDefaultCancelAcceptWaitMs = 2000;
// 等待会话收敛与槽位释放的预算。等满即说明实现有缺陷，按运行级失败报告，而不是继续等。
constexpr Milliseconds kSettleBudget{5000};
// 等待期间每次让出的时长。它是让出而不是“休息”：预算用尽即失败，因此不构成隐藏的时间假设。
constexpr Milliseconds kPollPause{1};

// 输出记录前缀。两种前缀是公共契约的一部分：调用方按前缀区分记录类型，不需要猜内容。
constexpr char kResponsePrefix[] = "response ";
constexpr char kEventPrefix[] = "event ";
constexpr std::size_t kResponsePrefixSize = 9;
constexpr std::size_t kEventPrefixSize = 6;

// 场景策略：把“该跑成什么样”与“该用什么入口参数”绑在一处，避免两处各写一份而将来分歧。
struct ScenarioPolicy {
  SessionAppInputMode mode = SessionAppInputMode::kText;
  const char* text = "";
  const char* wav_path = "";
  // 数据面待发上限（字节）。慢消费场景取小值，使“有界”在几步之内就能被观测到，
  // 而不必先产出几十 KiB 才触发。
  std::size_t max_pending_data_bytes = 4096;
  // 外部控制请求的等待预算（毫秒）。取消场景取 0：只受理、不等待清理，使“受理”与
  // “设备已经安静”在证据里天然分开。
  std::size_t cancel_wait_budget_ms = 2000;
  // 控制响应待发上限（字节）。慢消费场景取小值：控制响应是"每一行都算数"的记录，用它们
  // 填满队列比用数据事件更稳定——v1 的入口每轮只交付一条文本事件，数据量由回答长度决定，
  // 而控制响应的大小由协议固定，因此"几步之内必然填满"是可以计算的，不依赖回答写多长。
  std::size_t max_pending_control_bytes = 64U * 1024U;
};

ScenarioPolicy PolicyFor(const MockProfileConfig& config) {
  const MockProfileScenario scenario = config.scenario;
  ScenarioPolicy policy;
  switch (scenario) {
    case MockProfileScenario::kNormal:
      // 命中内置知识夹具的高分记录：走 L1 直答，不需要生成模型，整条链路的证据最短最稳定。
      policy.text = "the capital of france";
      break;
    case MockProfileScenario::kSlowConsumer:
      // 走 L2：会话会产生文本与音频，客户端却只按小预算取字节，因此"发不完"是一个由契约
      // 决定的必然事件，而不是依赖某一次调度恰好慢。
      policy.text = "the weather in paris";
      policy.max_pending_data_bytes = 4096;
      policy.max_pending_control_bytes = 512;
      break;
    case MockProfileScenario::kCancel:
      policy.text = "the weather in paris";
      policy.cancel_wait_budget_ms = 0;
      break;
    case MockProfileScenario::kFault:
      // 故意指向一个不存在的固定音频路径：输入在开始任何轮次之前就不可用，因此“故障”不依赖
      // 任何后端的异常行为，也不受本机文件系统内容影响。
      policy.mode = SessionAppInputMode::kWavFile;
      policy.wav_path = "nexweave-mock-profile/absent-fixture.wav";
      break;
  }
  // 输入路径的覆盖值优先于场景默认值：它只改变"这次运行读哪个文件"，不改变场景声明的形态。
  if (!config.wav_path.empty()) {
    policy.wav_path = config.wav_path.c_str();
  }
  return policy;
}

// 取字节预算。只有慢消费场景限速；其余场景返回一次 flush 的上界，也就是“不限速”——它们的
// 验收目标不是背压，人为限速只会让证据变长，而不会多证明任何东西。
//
// 形参是场景与预算值，而不是整份配置：这个判断只依赖这两项。传整份配置会让读代码的人以为
// 还有别的字段参与计算，也会让想绕过限速的调用方不得不伪造一份“场景改成 normal”的配置。
std::size_t EffectiveDrainBudget(MockProfileScenario scenario, std::size_t configured) {
  if (scenario != MockProfileScenario::kSlowConsumer) {
    return kFlushChunk;
  }
  return configured;
}

std::string ScenarioRequestId(MockProfileScenario scenario) {
  return std::string("req-") + mock_profile_scenario_slug(scenario) + "-start";
}

std::string ScenarioCancelRequestId(MockProfileScenario scenario) {
  return std::string("req-") + mock_profile_scenario_slug(scenario) + "-cancel";
}

// 任务身份必须满足 v1 的 work_id 契约（"w-" 后跟至少一位十进制数字）。场景标识是名字而
// 不是数字，因此这里用场景在枚举里的序号：每个场景一个固定数字，重复执行永远得到同一个
// 身份，而不同场景不会共用同一个任务身份。
std::string ScenarioWorkId(MockProfileScenario scenario) {
  return std::string("w-") + std::to_string(static_cast<unsigned>(scenario) + 1);
}

std::string ScenarioSessionId(MockProfileScenario scenario) {
  return std::string("session-") + mock_profile_scenario_slug(scenario);
}

// 内置的确定性检索夹具：两条记录覆盖 L1 直答与 L2 携带上下文生成。分数是排序量，不是概率。
std::vector<RetrievedChunk> BuiltinKnowledge() {
  return {
      RetrievedChunk{"geo-capital-fr", "the capital of france is paris", 0.97},
      RetrievedChunk{"weather-paris", "the weather in paris is sunny and dry today", 0.60},
  };
}

// 播放暂停闸门：把"会话正在执行"从一个瞬时状态变成调用方可以把握的窗口。
//
// 它解决什么问题
// --------------
// 取消场景要验证的是"打断一个正在执行的会话"，而确定性夹具跑完一轮只需不到一毫秒：只靠
// 轮询或墙钟去等，取消请求几乎总是落在会话收尾之后，验收就变成了"这次跑得快不快"。闸门把
// 时序交回给调用方：会话在已经写出第 N 帧之后停下来等放行，调用方由此获得一个**确定存在**
// 的在途窗口，可以在其中提交取消。
//
// 状态机与职责
// ------------
//   Armed：运行开始前由调用方置位，并给出"停在第几帧"。同一份配置可以多次重置（每次运行
//          前必须重置，否则上一轮的门会立刻放行，窗口消失）。
//   Hit：  第 N 帧写出之后到达。闸门在这里通知等待者，并阻塞会话线程直到放行。
//   Passed：放行之后不再拦任何帧，本轮后续帧直接写出。
//
// 阻塞与超时：等待放行不设内部超时——超时属于调用方的等待预算，由调用方在自己的有界等待
// 里决定放弃（那时它会走运行级失败，而不是让闸门自己把会话放走）。等待期间不持锁、不写
// 设备、不分配。
//
// 线程归属：会话线程在 reach() 里阻塞，调用方线程在 wait_until_hit()/release() 里等待与
// 放行，两者只用一把互斥量与条件变量交接状态。不创建线程、不打开设备。
class PauseGate {
 public:
  // 布置一次暂停：stop_after_frame 之后（该帧已经写出）拦住下一帧。0 表示不拦。
  void arm(std::size_t stop_after_frame) {
    std::lock_guard<std::mutex> guard(mutex_);
    stop_after_frame_ = stop_after_frame;
    hit_ = false;
    released_ = stop_after_frame == 0;
  }

  // 会话线程每写出帧后调用一次：写出帧数达到闸门位置时置位并阻塞到放行。
  void reach(std::size_t written_frames) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (released_ || written_frames < stop_after_frame_) {
      return;
    }
    hit_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this] { return released_; });
  }

  // 调用方等待"闸门已经到达"。返回 false 表示预算内没有到达（会话可能已经收敛）。
  bool wait_until_hit(std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, budget, [this] { return hit_ || released_; });
  }

  // 放行并解除拦截：幂等，可重复调用。
  void release() {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t stop_after_frame_ = 0;
  bool hit_ = false;
  bool released_ = true;
};

// 带闸门的音频汇：把帧转交给真正的汇，并在到达闸门位置时暂停会话线程。
//
// 为什么包在汇上而不是包在播放组件上：汇是"已经写出设备"这一事实的边界，闸门因此与
// "帧确实交付过"对齐，而不是与"帧被播放组件接收"对齐。
class PauseSink final : public capability::IAudioSink {
 public:
  PauseSink(FakeAudioSink& inner, PauseGate& gate) : inner_(inner), gate_(gate) {}

  void arm(std::size_t stop_after_frame) { gate_.arm(stop_after_frame); }
  bool wait_until_hit(std::chrono::milliseconds budget) { return gate_.wait_until_hit(budget); }
  void release() { gate_.release(); }

  nexweave::domain::OperationResult open() override { return inner_.open(); }
  nexweave::domain::OperationResult write(const nexweave::domain::AudioFrame& frame) override {
    const nexweave::domain::OperationResult written = inner_.write(frame);
    if (written.ok()) {
      gate_.reach(inner_.frames().size());
    }
    return written;
  }
  nexweave::domain::OperationResult close() noexcept override { return inner_.close(); }
  nexweave::domain::OperationResult cancel() noexcept override { return inner_.cancel(); }

 private:
  FakeAudioSink& inner_;
  PauseGate& gate_;
};
// 进程内能力夹具。它拥有全部能力对象，并通过引用把它们交给会话工厂；成员声明顺序即构造
// 顺序，也就是依赖顺序（被借用的对象先于借用者建立）。
class MockProfileFixture {
 public:
  MockProfileFixture()
      : rag_(BuiltinKnowledge()),
        router_(rag_),
        llm_({"第一句回答。", "第二句回答。", "第三句回答。"}),
        pause_sink_(sink_, gate_),
        playback_(pause_sink_, clock_) {
    // 音频汇由夹具拥有，因此"打开设备"这一步也由夹具在会话开始之前完成：播放组件只负责把
    // 帧写出去，设备未打开时每一次写入都会以设备错误告终。这一步放在构造期而不是每次会话，
    // 是因为"同一输入流只有一个生产者"同样适用于输出：整个 profile 只有一个音频汇。
    const OperationResult opened = sink_.open();
    // 构造期没有调用方可以接收错误；失败说明夹具自身状态不对（同一个汇被重复打开），属于
    // 实现缺陷。用断言而不是静默继续，否则缺陷会被伪装成"某一轮设备失败"。
    assert(opened.ok());
    (void)opened;
  }

  MockProfileFixture(const MockProfileFixture&) = delete;
  MockProfileFixture& operator=(const MockProfileFixture&) = delete;

  nexweave::capability::IAsr& asr() { return asr_; }
  nexweave::capability::IRag& rag() { return rag_; }
  FakeRagRouter& router() { return router_; }
  nexweave::capability::ITts& tts() { return tts_; }
  nexweave::capability::ILlm& llm() { return llm_; }
  nexweave::runtime::IAudioPlayback& playback() { return playback_; }

  // 已经写进音频汇的帧数。取消场景用它作为“确实已经出声”的判据：这是播放组件自己记账的
  // 事实，而不是通过请求入口观察到的——v1 的入口不交付逐帧 PCM，因此帧数只能在这一层看。
  std::size_t frames_written() const { return sink_.frames().size(); }

  // 播放暂停闸门：取消场景用它把“会话正在执行”变成一个确定存在的窗口，而不是靠抢时序。
  void arm_pause(std::size_t stop_after_frame) { pause_sink_.arm(stop_after_frame); }
  bool wait_for_pause(std::chrono::milliseconds budget) {
    return pause_sink_.wait_until_hit(budget);
  }
  void release_pause() { pause_sink_.release(); }

 private:
  // 文本模式不使用常驻输入，因此这里不建立 ResidentAudioInput：模拟常驻输入与语音分段由
  // session_app 与常驻输入测试覆盖，本层不重复一遍。
  // ASR 夹具的假设列表必须非空（空列表会被判定为配置错误）；文本模式下最终文本来自会话的
  // 文本注入器，这个假设只用于让识别能力本身合法。
  FakeAsr asr_{std::vector<std::string>{"the capital of france"}};
  FakeRag rag_;
  FakeRagRouter router_;
  FakeTts tts_;
  FakeLlm llm_;
  // 逻辑时钟不自增：播放是否“已经播完”由它决定，而本层不依赖真实时间。会话的收尾条件不
  // 包含播放完成，因此固定时钟不改变收敛性，只让“合成结束”与“播放结束”保持可区分。
  // 声明顺序即构造顺序：闸门与汇必须先于包装它们的 PauseSink，PauseSink 与时钟必须先于
  // 播放组件（播放组件借用它们）。析构顺序相反，因此会话收敛之后这些对象都还在。
  PauseGate gate_;
  FakeAudioSink sink_;
  PauseSink pause_sink_;
  ManualPlaybackClock clock_;
  LogicalClockPlayback playback_;
};

// 把配置压成规范字符串。只包含影响结果的字段，顺序固定，因此同一配置永远得到同一份文本。
// 场景旋钮也在内：它们确实改变运行行为，若不算进去，两次行为不同的运行会得到同一个指纹。
std::string CanonicalConfig(const MockProfileConfig& config) {
  std::string text;
  text += "scenario=";
  text += to_string(config.scenario);
  text += "\nstream_id=";
  text += config.stream_id;
  text += "\ncancel_after_pcm_events=";
  text += std::to_string(config.cancel_after_pcm_events);
  text += "\ndrain_budget_bytes=";
  text += std::to_string(EffectiveDrainBudget(config.scenario, config.drain_budget_bytes));
  text += "\n";
  return text;
}

// 输入指纹：本场景实际喂给会话的固定输入。不含硬件与本机路径，因此跨机器可比。
std::string CanonicalInput(const MockProfileConfig& config) {
  const ScenarioPolicy policy = PolicyFor(config);
  std::string text;
  text += "mode=";
  text += nexweave::runtime::to_string(policy.mode);
  text += "\ntext=";
  text += policy.text;
  text += "\nwav=";
  text += policy.wav_path;
  text += "\nknowledge=geo-capital-fr@0.97,weather-paris@0.60\n";
  return text;
}

// 确定性内容哈希（FNV-1a，64 位）。
//
// 用途与边界：它是**关联键**，用于把两次运行归到同一份配置或输入上；它不是密码学摘要，
// 不提供抗碰撞保证，也不作为任何安全边界。之所以自己实现而不引入依赖：Mock profile 不引入
// 网络、模型或加密库，而“同一输入必得同一值”只需要一个固定算法，不需要强度。
std::string ContentHash(const std::string& text) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char raw : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(raw));
    hash *= 1099511628211ULL;
  }
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
  return std::string(buffer);
}

std::string Fingerprint(const std::string& text) {
  return std::string("fnv1a64:") + ContentHash(text);
}

// 错误码的稳定文本名，供汇总与证据使用。本地实现而不扩张领域层的公共接口：领域层只导出
// 错误码与其合法性校验，把“给证据看的名字”加进去会让领域层多背一份展示责任。
std::string ErrorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::kNone:
      return "none";
    case ErrorCode::kInvalidInput:
      return "invalid_input";
    case ErrorCode::kMissingField:
      return "missing_field";
    case ErrorCode::kAlreadyCompleted:
      return "already_completed";
    case ErrorCode::kCancelled:
      return "cancelled";
    case ErrorCode::kTimeout:
      return "timeout";
    case ErrorCode::kBackendFailure:
      return "backend_failure";
    case ErrorCode::kDeviceFailure:
      return "device_failure";
    case ErrorCode::kBusy:
      return "busy";
  }
  return "unknown";
}

// 计入一条已经解码的数据事件。计数的单一来源在这里，因此“交付了几条”与“记录了几条”不会
// 出现两份实现。事件类型只区分文本与终态：v1 的请求入口不交付逐帧 PCM（它只交付逐轮文本与
// 终态），因此这里没有 PCM 分支——为一个不会出现的类型维护计数只会产生永远为 0 的字段。
void AccountEvent(const nexweave::protocol::DataEvent& event, MockProfileResult& result) {
  ++result.event_count;
  if (event.type == DataEventType::kToken) {
    ++result.token_event_count;
  }
  if (event.end) {
    ++result.terminal_event_count;
    result.terminal_error = Error{event.error_code, event.message};
    result.observed.session_completed = event.type == DataEventType::kDone;
    result.observed.failure_event = event.type == DataEventType::kError;
  } else if (event.type == DataEventType::kError && event.error_code == ErrorCode::kCancelled) {
    // 局部取消：某一轮被取消但会话继续。它不结束会话，因此只参与形态判定。
    result.observed.cancellation_observed = true;
  }
}

// 解析一条完整的输出行并计入结果。
//
// 输入是**入口直接交出的原始报文**（不含任何前缀）：入口的发送队列里放的就是协议层编码结果，
// 它不替调用方决定"这一行是什么"。因此前缀由本层在归类的同一处补上——一次解码同时决定
// 记录类型与前缀，两者不会各写一份而将来分歧。
//
// 返回 false 表示这一行既不是控制响应也不是数据事件，属于实现缺陷（协议层与本层的版本
// 不一致）；此时不把它伪装成任何一类记录。
bool ConsumeLine(const std::string& line, std::vector<std::string>& records,
                 MockProfileResult& result) {
  const auto response = nexweave::protocol::decode_response(line);
  if (response.ok()) {
    ++result.response_count;
    result.observed.session_accepted =
        result.observed.session_accepted || response.value->result.ok();
    records.push_back(std::string(kResponsePrefix) + line);
    return true;
  }
  const auto event = nexweave::protocol::decode_event_metadata(line);
  if (event.ok()) {
    AccountEvent(*event.value, result);
    records.push_back(std::string(kEventPrefix) + line);
    return true;
  }
  return false;
}

// 在一轮里把待发字节按预算取完。返回取走的字节数；0 表示当前没有更多可取的字节。
//
// 关键顺序：flush 交出的字节是**已经带好记录前缀的完整行**，因此必须先按行切分、把每一行
// 交给 ConsumeLine 归类，再把整行记进输出。若直接把原始字节记进输出，记录就会丢掉前缀，
// 调用方再也分不清哪一行是控制响应、哪一行是数据事件——那正是本层要提供的可读性。
//
// 未成帧的尾部字节留在 buffer 里等下一次取字节补齐；本层不假设“一次 flush 交出整数条消息”。
std::size_t DrainOnce(Gateway& gateway, ConnectionId connection, std::size_t budget,
                      std::string& buffer, std::vector<std::string>& records,
                      MockProfileResult& result) {
  std::size_t taken = 0;
  std::string chunk;
  for (;;) {
    chunk.clear();
    const std::size_t got = gateway.flush(connection, chunk, budget);
    if (got == 0) {
      break;
    }
    taken += got;
    buffer.append(chunk);
    std::size_t position = 0;
    for (;;) {
      const std::size_t newline = buffer.find('\n', position);
      if (newline == std::string::npos) {
        break;
      }
      const std::string line = buffer.substr(position, newline - position);
      position = newline + 1;
      if (line.empty()) {
        continue;
      }
      if (!ConsumeLine(line, records, result)) {
        result.error = Error{ErrorCode::kBackendFailure, "输出里出现了无法归属的记录"};
        break;
      }
    }
    buffer.erase(0, position);
    if (!result.error.ok()) {
      break;
    }
    // 预算已经用完就没有必要再调一次 flush：下一次调用只会返回 0。提前退出使“每轮取走多少”
    // 与配置严格一致，慢消费场景的触发点因此完全由配置决定。
    if (taken >= budget) {
      break;
    }
  }
  return taken;
}

// 驱动连接直到收敛：连接被服务端关闭、收到终态事件、或等待预算用尽。
//
// 顺序理由：先 flush 再判断状态。若先判断状态，最后一次投递的字节会留在队列里没被取走，
// 于是“连接被关闭”这件事会看起来像“事件没有产生”。等待只在确实还没有终态、连接仍打开时
// 发生，因此正常场景不会因为多等一轮而变慢。
bool PumpConnection(const MockProfileConfig& config, Gateway& gateway, ConnectionId connection,
                    std::string& buffer, std::vector<std::string>& records,
                    MockProfileResult& result) {
  const std::size_t budget = EffectiveDrainBudget(config.scenario, config.drain_budget_bytes);
  const auto deadline = std::chrono::steady_clock::now() + kSettleBudget;
  for (;;) {
    DrainOnce(gateway, connection, budget, buffer, records, result);
    if (!result.error.ok()) {
      return false;
    }
    if (result.terminal_event_count > 0) {
      return true;
    }
    if (!gateway.connection_stats(connection).open) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return true;
    }
    std::this_thread::sleep_for(kPollPause);
  }
}
// 会话是否已经收敛、可以交付结果。交付本身是非阻塞判断，因此这里只提供一个等待上界。
bool WaitForSessionSettled(Gateway& gateway) {
  const auto deadline = std::chrono::steady_clock::now() + kSettleBudget;
  while (gateway.status().session_in_flight) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    gateway.deliver_settled();
    std::this_thread::sleep_for(kPollPause);
  }
  return true;
}

// 槽位是否已经停止推进（回到空闲、不可用或已退出）。cancel 之后仍要等这一步：“清理完成”
// 与“取消受理”是两件分开的事实，只有它成立才说明资源已经交还。
bool WaitForSlotSettled(Supervisor& supervisor) {
  const auto deadline = std::chrono::steady_clock::now() + kSettleBudget;
  for (;;) {
    const SupervisorState state = supervisor.status().state;
    if (state == SupervisorState::kIdle || state == SupervisorState::kUnavailable ||
        state == SupervisorState::kClosed) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(kPollPause);
  }
}
// 在取消场景里等待"停止已经被受理"。
//
// 两条顺序理由：
//   1. 必须持续驱动入口。取消请求写进连接之后，入口只有在有人取字节时才会读到并处理它；
//      只盯监督器状态空等，请求会一直躺在队列里，等来的只会是"会话自然收敛"。
//   2. 等待期间取字节不受慢消费预算限制。取消场景的验收目标是"停止被受理"，不是背压；
//      若沿用限速读取，入口先把几十条输出事件铺满队列，取消响应就会排在它们后面，等到的
//      是超时而不是受理——那会把一个正确的实现判成失败。
bool WaitForCancelAccepted(Gateway& gateway, ConnectionId connection, Supervisor& supervisor,
                           std::string& buffer, std::vector<std::string>& records,
                           MockProfileResult& result, std::size_t budget_ms) {
  const auto accepted = [&supervisor]() {
    const auto status = supervisor.status();
    return status.cancel_accepted || status.sessions_cancelled > 0;
  };
  const auto deadline = std::chrono::steady_clock::now() + Milliseconds{budget_ms};
  for (;;) {
    if (accepted()) {
      return true;
    }
    // 等待期间不限速：取消场景的验收目标是“停止被受理”，不是背压。沿用限速读取会让入口先
    // 把几十条输出事件铺满队列，取消响应排在它们后面，等到的就是超时——那会把一个正确的
    // 实现判成失败。
    DrainOnce(gateway, connection, kFlushChunk, buffer, records, result);
    if (!result.error.ok()) {
      return false;
    }
    if (accepted()) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(kPollPause);
  }
}

bool EncodeFrame(const ControlRequest& request, std::string& frame) {
  const auto encoded = nexweave::protocol::encode_request(request);
  if (!encoded.ok()) {
    return false;
  }
  frame = *encoded.value;
  frame.push_back('\n');
  return true;
}

// 操作名按 const char* 接收而不是 const std::string&：调用点传的是字面量，若形参是
// 常量字符串引用，就会先构造一个临时 std::string 再绑定；那个临时只活到函数返回，把它
// 赋给 request.operation 会留下悬空内容，表现为"请求无法编码"这种与真实原因无关的失败。
ControlRequest MakeRequest(MockProfileScenario scenario, const std::string& request_id,
                           const char* operation, std::size_t deadline_ms) {
  ControlRequest request;
  request.request_id = request_id;
  request.operation = operation;
  request.work_id = ScenarioWorkId(scenario);
  request.session_id = ScenarioSessionId(scenario);
  // deadline 必须为正（协议层校验）；它是接收方的处理预算，不是本层的等待上界。
  request.deadline = Milliseconds{deadline_ms == 0 ? 1 : deadline_ms};
  return request;
}
// 输出产物的文件名。它们写在配置给出的目录下；目录本身由调用方持有，因此运行结束只删除
// 文件，不删除目录——删除别人的目录不是本层的权限范围。
constexpr char kEventsFileName[] = "events.jsonl";
constexpr char kSummaryFileName[] = "summary.json";
constexpr char kManifestFileName[] = "run-manifest.json";

// 写文件并返回是否成功。失败不抛异常：它是一次运行级失败，应当出现在结果里而不是变成崩溃。
bool WriteFile(const std::string& path, const std::string& content) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const std::size_t written = std::fwrite(content.data(), 1, content.size(), file);
  const int closed = std::fclose(file);
  return written == content.size() && closed == 0;
}

// 删除文件。文件本来就不存在也算成功：清理的目标是“没有残留”，不是“必须删掉了什么”。
bool RemoveFile(const std::string& path) {
  if (std::remove(path.c_str()) == 0) {
    return true;
  }
  // 区分“本来就没有”与“删不掉”：前者是干净的，后者必须如实报告为残留。
  std::FILE* probe = std::fopen(path.c_str(), "rb");
  if (probe == nullptr) {
    return true;
  }
  std::fclose(probe);
  return false;
}

// 由配置构造出足以复现本次运行的命令行。可选参数只在真的偏离默认值时才写出，使清单里的
// 命令既完整又不啰嗦；输出目录属于"这次跑写到哪"，同样写入。
std::string BuildCommandLine(const MockProfileConfig& config, const std::string& scenario_name) {
  std::string command = "nexweave_mock_profile --scenario " + scenario_name;
  command += " --stream-id " + config.stream_id;
  command += " --cancel-after-pcm " + std::to_string(config.cancel_after_pcm_events);
  command += " --drain-budget " + std::to_string(config.drain_budget_bytes);
  if (!config.output_dir.empty()) {
    command += " --out-dir " + config.output_dir;
  }
  return command;
}

std::string JoinPath(const std::string& directory, const char* name) {
  if (directory.empty()) {
    return std::string(name);
  }
  const char last = directory.back();
  const bool separated = last == '/' || last == '\\';
  return directory + (separated ? "" : "/") + name;
}

// 确保输出目录存在。目录不存在时创建；创建失败按"写不进去"处理，由调用方升级为运行级失败。
// 之所以由本层创建而不是只要求调用方准备好：目录是本次运行的产物容器，与产物同属一份职责；
// 而且"目录不存在"在真实调用里是最常见的失败原因，让它静默变成"0 个产物写入"会误导排查。
bool EnsureDirectory(const std::string& directory) {
  if (directory.empty()) {
    return true;
  }
  std::error_code error;
  // create_directories 对已存在的目录返回假且不报错，因此这一个调用同时覆盖"已存在"与
  // "需要新建"两种情形，不需要先探测再创建——先探测再创建会引入一次多余的竞态窗口。
  std::filesystem::create_directories(directory, error);
  if (error) {
    return false;
  }
  return std::filesystem::is_directory(directory, error) && !error;
}

// 把本次运行的产物写入输出目录。返回成功写入的文件数。
std::size_t WriteArtifacts(const MockProfileConfig& config, const MockProfileResult& result) {
  if (config.output_dir.empty()) {
    return 0;
  }
  if (!EnsureDirectory(config.output_dir)) {
    return 0;
  }
  std::string events;
  for (const std::string& record : result.records) {
    events += record;
    events.push_back('\n');
  }
  std::size_t written = 0;
  written += WriteFile(JoinPath(config.output_dir, kEventsFileName), events) ? 1 : 0;
  written +=
      WriteFile(JoinPath(config.output_dir, kSummaryFileName), result.summary_json + "\n") ? 1 : 0;
  written += WriteFile(JoinPath(config.output_dir, kManifestFileName),
                       result.manifest_json + "\n")
                 ? 1
                 : 0;
  return written;
}

// 删除本次运行写入的产物。返回“目录里已经没有本次产物”是否成立。
bool RemoveArtifacts(const MockProfileConfig& config) {
  if (config.output_dir.empty()) {
    return true;
  }
  bool clean = true;
  clean = RemoveFile(JoinPath(config.output_dir, kEventsFileName)) && clean;
  clean = RemoveFile(JoinPath(config.output_dir, kSummaryFileName)) && clean;
  clean = RemoveFile(JoinPath(config.output_dir, kManifestFileName)) && clean;
  return clean;
}

// 观察到的形态是否与预期一致；不一致时把第一处差异的字段名写进 mismatch，使失败的用例
// 与失败的运行都能直接指出“哪一条不成立”，而不是只给一个布尔。
bool CompareExpectation(const MockProfileExpectation& expected,
                        const MockProfileExpectation& observed, std::string& mismatch) {
  const std::pair<const char*, bool> checks[] = {
      {"session_accepted", expected.session_accepted == observed.session_accepted},
      {"session_completed", expected.session_completed == observed.session_completed},
      {"failure_event", expected.failure_event == observed.failure_event},
      {"cancellation_observed", expected.cancellation_observed == observed.cancellation_observed},
      {"audio_rendered", expected.audio_rendered == observed.audio_rendered},
      {"connection_closed_by_server",
       expected.connection_closed_by_server == observed.connection_closed_by_server},
      {"slow_client_close", expected.slow_client_close == observed.slow_client_close},
      {"text_delivered", expected.text_delivered == observed.text_delivered},
  };
  for (const auto& check : checks) {
    if (!check.second) {
      mismatch = check.first;
      return false;
    }
  }
  mismatch.clear();
  return true;
}

// 关闭原因是否表示“服务端主动关闭”。对端断开不算：那是客户端的行为，不能用来证明服务端的
// 有界策略生效。
bool IsServerInitiatedClose(GatewayCloseReason reason) {
  return reason == GatewayCloseReason::kSlowClient ||
         reason == GatewayCloseReason::kProtocolViolation ||
         reason == GatewayCloseReason::kOversizedFrame || reason == GatewayCloseReason::kExited;
}

}  // namespace
const char* to_string(MockProfileScenario scenario) noexcept {
  switch (scenario) {
    case MockProfileScenario::kNormal:
      return "normal";
    case MockProfileScenario::kSlowConsumer:
      return "slow";
    case MockProfileScenario::kCancel:
      return "cancel";
    case MockProfileScenario::kFault:
      return "fault";
  }
  return "";
}

bool parse_mock_profile_scenario(std::string_view name, MockProfileScenario& scenario) noexcept {
  // to_string 是场景名称的唯一来源：新增场景时只需要改这一处，命令行解析与预期表都不会漂移。
  const MockProfileScenario candidates[] = {MockProfileScenario::kNormal,
                                            MockProfileScenario::kSlowConsumer,
                                            MockProfileScenario::kCancel,
                                            MockProfileScenario::kFault};
  for (const MockProfileScenario candidate : candidates) {
    if (name == to_string(candidate)) {
      scenario = candidate;
      return true;
    }
  }
  return false;
}

const char* mock_profile_scenario_slug(MockProfileScenario scenario) noexcept {
  return to_string(scenario);
}

MockProfileExpectation mock_profile_expectation(MockProfileScenario scenario) noexcept {
  MockProfileExpectation expectation;
  switch (scenario) {
    case MockProfileScenario::kNormal:
      break;
    case MockProfileScenario::kSlowConsumer:
      // 被关闭的是连接，不是会话：客户端取走字节的速度跟不上产出，因此终态事件没有接收者。
      // 会话本身是否跑成由监督器账目回答（见 run_mock_profile 里的核对），不在这里猜。
      expectation.connection_closed_by_server = true;
      expectation.slow_client_close = true;
      expectation.session_completed = false;
      expectation.text_delivered = false;
      break;
    case MockProfileScenario::kCancel:
      expectation.session_completed = false;
      expectation.failure_event = true;
      expectation.cancellation_observed = true;
      break;
    case MockProfileScenario::kFault:
      expectation.session_completed = false;
      expectation.failure_event = true;
      expectation.audio_rendered = false;
      expectation.text_delivered = false;
      break;
  }
  return expectation;
}

OperationResult validate_mock_profile_config(const MockProfileConfig& config) {
  if (config.stream_id.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "stream_id 不能为空");
  }
  // 场景标识必须是自己认得的枚举值：越界值来自强制转换或反序列化错误。若不在这里拒绝，
  // 后面的 switch 会落进默认分支而悄悄按另一个场景运行——那会造出“看起来跑过”的假证据。
  bool known = false;
  for (const MockProfileScenario candidate :
       {MockProfileScenario::kNormal, MockProfileScenario::kSlowConsumer,
        MockProfileScenario::kCancel, MockProfileScenario::kFault}) {
    known = known || candidate == config.scenario;
  }
  if (!known) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "未知的 profile 场景");
  }
  // 旋钮与场景必须自洽：把取消旋钮写在正常场景上，说明“这条命令到底在验什么”已经不可解释，
  // 因此按配置错误拒绝，而不是默默忽略它。
  if (config.scenario != MockProfileScenario::kCancel && config.cancel_after_pcm_events != 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "只有取消场景可以使用 cancel_after_pcm_events");
  }
  // 取消场景必须给出正的打断点：见 MockProfileConfig::cancel_after_pcm_events 的说明——
  // 不设等待会让“取消”与“会话自然收尾”抢时序，同一配置时而生效时而失效。
  if (config.scenario == MockProfileScenario::kCancel && config.cancel_after_pcm_events == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "取消场景必须给出大于 0 的打断点");
  }
  if (config.scenario == MockProfileScenario::kSlowConsumer && config.drain_budget_bytes == 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "慢消费场景必须给出正的取字节预算");
  }
  if (config.scenario != MockProfileScenario::kSlowConsumer && config.drain_budget_bytes != 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "只有慢消费场景可以使用 drain_budget_bytes");
  }
  return OperationResult::success();
}
MockProfileResult run_mock_profile(const MockProfileConfig& config) {
  MockProfileResult result;
  result.scenario = config.scenario;
  result.scenario_name = to_string(config.scenario);
  // 观察字段的初值取“什么都没发生”：默认值若照抄预期，一次没跑起来的运行会被报告成符合预期。
  result.observed.session_accepted = false;
  result.observed.session_completed = false;
  result.observed.failure_event = false;
  result.observed.cancellation_observed = false;
  result.observed.audio_rendered = false;
  result.observed.connection_closed_by_server = false;
  result.observed.slow_client_close = false;
  result.observed.text_delivered = false;

  const std::string config_text = CanonicalConfig(config);
  const std::string input_text = CanonicalInput(config);

  // 配置校验在任何资源建立之前完成：失败时结果里没有连接、没有线程、没有文件，调用方拿到
  // 的是一份“什么都没发生”的账目，而不是一次跑了一半的运行。
  const OperationResult valid = validate_mock_profile_config(config);
  if (!valid.ok()) {
    result.error = valid.error;
    result.ledger.config_valid = false;
  } else {
    const ScenarioPolicy policy = PolicyFor(config);

    MockProfileFixture fixture;
    SessionAppConfig app_config;
    app_config.mode = policy.mode;
    app_config.stream_id = config.stream_id;
    app_config.text = policy.text;
    app_config.wav_path = policy.wav_path;

    // 工厂借用夹具的全部能力；它同时充当“最近一次会话结果”的来源。
    SessionAppOwnerFactory factory(app_config, fixture.asr(), fixture.rag(), fixture.router(),
                                   fixture.tts(), fixture.playback(), nullptr, &fixture.llm());

    SupervisorConfig supervisor_config;
    supervisor_config.start_wait_budget = kSettleBudget;
    supervisor_config.cleanup_wait_budget = kSettleBudget;
    Supervisor supervisor(factory, supervisor_config);

    GatewayConfig gateway_config;
    gateway_config.max_pending_data_bytes = policy.max_pending_data_bytes;
    gateway_config.max_pending_control_bytes = policy.max_pending_control_bytes;
    gateway_config.cancel_wait_budget = Milliseconds{policy.cancel_wait_budget_ms};
    gateway_config.exit_wait_budget = kSettleBudget;
    Gateway gateway(supervisor, factory, gateway_config);

    const ConnectionId connection = gateway.open_connection();
    if (connection == kInvalidConnectionId) {
      result.error = Error{ErrorCode::kBackendFailure, "请求入口拒绝建立连接"};
    } else {
      ++result.ledger.connections_opened;
      // 受理一次创建就会让监督器建立一个工作线程。这里记账而不是事后查询：账目要回答的是
      // “本层创建了什么”，而不是“退出时还剩什么”。
      result.ledger.threads_created = 1;
      std::string buffer;
      std::string frame;
      // 控制请求自带的处理预算是“接收方从收到它开始可用多久”，与入口自己的等待预算是两件
      // 事。这里的余量必须明显大于取消等待预算，否则入口可能在处理取消的途中就判定请求超时，
      // 把一次被受理的取消报成未受理。余量因此是一个独立的常量，而不是让两个概念共用一个算式。
      constexpr std::size_t kRequestDeadlineMarginMs = 3000;
      const std::size_t deadline_ms = policy.cancel_wait_budget_ms + kRequestDeadlineMarginMs;

      // 取消场景必须在**提交创建之前**布好闸门：会话跑得比任何轮询都快，先提交再布置就会
      // 出现"会话已经跑完、闸门才装好"的窗口，场景于是退化成一次碰运气。布置之后再提交，
      // 会话无论跑多快都必然先撞上闸门。
      //
      // 打断点必须是正数（配置校验保证）：不设等待就等于让取消请求与会话收尾抢时序，
      // 那正是本场景要消除的不确定性。
      if (config.scenario == MockProfileScenario::kCancel) {
        fixture.arm_pause(config.cancel_after_pcm_events);
      }

      // 1) 受理一次创建。受理响应与执行终态是两件事，因此这里只提交、不等会话。
      const ControlRequest start_request =
          MakeRequest(config.scenario, ScenarioRequestId(config.scenario), "start", deadline_ms);
      if (!EncodeFrame(start_request, frame)) {
        result.error = Error{ErrorCode::kBackendFailure, "创建请求无法编码"};
      } else {
        gateway.feed(connection, frame);
      }

      // 慢消费场景：先把受理响应按本场景的预算取走一次。受理响应是"这一次创建被接受了"的
      // 唯一凭据，若它随连接一起被关闭丢掉，整条证据就只剩"连接被关了"，无法区分"会话被受理
      // 之后发不完"与"请求根本没被受理"。取走它不改变慢消费的结论：队列仍然只增不减。
      if (result.error.ok() && config.scenario == MockProfileScenario::kSlowConsumer) {
        // 本场景的取字节预算是 8 字节，而一条控制响应有上百字节，因此"取到受理响应"需要
        // 多轮。循环以"确实解析出一条响应"为退出条件，而不是以"缓冲空了"：队列随后会被
        // 查询响应持续填满，等它空下来就等于放弃这个场景。上界只是防止"响应永远不成帧"
        // 时挂起，不参与正常路径。
        for (std::size_t round = 0; round < 1024 && result.response_count == 0; ++round) {
          DrainOnce(gateway, connection, EffectiveDrainBudget(config.scenario, config.drain_budget_bytes), buffer, result.records,
                    result);
          if (!result.error.ok()) {
            break;
          }
        }
      }

      // 2) 取消场景：先把闸门布置在“第 cancel_after_pcm_events 帧已经写出之后”，再等它真的
      //    到达，然后才提交取消。会话跑到闸门就会停住，因此取消请求必然落在一个**在途**会话
      //    上——这不是靠运气抢到的窗口，而是由闸门制造的确定状态。
      //    旋钮为 0 表示不设闸门、立即受理，即最保守的“用户马上喊停”，同样合法。
      if (result.error.ok() && config.scenario == MockProfileScenario::kCancel) {
        {
          // 等待闸门的同时必须继续驱动入口：受理响应还在待发队列里，不取走它这份证据就
          // 缺了一条记录。取字节不会打扰会话线程，两者之间只有入口自己的锁。
          const auto pause_deadline = std::chrono::steady_clock::now() + kSettleBudget;
          bool paused = false;
          for (;;) {
            DrainOnce(gateway, connection, EffectiveDrainBudget(config.scenario, config.drain_budget_bytes), buffer, result.records,
                      result);
            paused = fixture.wait_for_pause(Milliseconds{1});
            if (paused || !result.error.ok()) {
              break;
            }
            if (std::chrono::steady_clock::now() >= pause_deadline) {
              break;
            }
          }
          if (!paused && result.error.ok()) {
            result.error = Error{ErrorCode::kTimeout, "会话没有在预算内到达取消打断点"};
          }
        }
        if (result.error.ok()) {
          // 受理停止那一刻已经写出的帧数：旋钮为 0 时它是 0，正好回答"取消发生在出声之前"。
          result.cancel_frames = fixture.frames_written();
          const ControlRequest cancel_request = MakeRequest(
              config.scenario, ScenarioCancelRequestId(config.scenario), "cancel", deadline_ms);
          if (!EncodeFrame(cancel_request, frame)) {
            result.error = Error{ErrorCode::kBackendFailure, "取消请求无法编码"};
          } else {
            gateway.feed(connection, frame);
            const std::size_t accept_wait = kDefaultCancelAcceptWaitMs;
            // 无论受理成功、超时还是出错，都必须放行闸门：否则会话线程会一直停在闸门上，
            // 后续“等收敛”会以超时告终，真实原因反而从证据里消失。
            const bool accepted_now = WaitForCancelAccepted(
                gateway, connection, supervisor, buffer, result.records, result, accept_wait);
            fixture.release_pause();
            if (!accepted_now) {
              if (result.error.ok()) {
                result.error = Error{ErrorCode::kTimeout, "取消没有被受理"};
              }
            } else {
              result.observed.cancellation_observed = true;
            }
          }
        }
      }
      // 3) 等会话收敛并交付结果。交付是“把已经定局的会话结论转成事件”，不是继续推理。
      if (result.error.ok()) {
        if (config.scenario == MockProfileScenario::kSlowConsumer) {
          // 慢消费：会话照常执行，客户端却几乎不取字节——每一轮只取走 drain budget 允许的
          // 那么一点，剩下的留在入口的有界队列里。队列达到声明上限时入口关闭连接并记录
          // 原因，这就是"缓冲不会随输出无界增长"的可观测形式。
          //
          // 循环上界是连接状态本身：连接关闭后下面两个条件都不再成立，循环自然退出，不需要
          // 额外的计数或超时。仍然保留一个时间上界，使"实现没有关闭连接"表现为一次明确失败
          // 而不是挂起。
          const auto settle_deadline = std::chrono::steady_clock::now() + kSettleBudget;
          std::size_t query_index = 0;
          while (gateway.connection_stats(connection).open) {
            DrainOnce(gateway, connection, EffectiveDrainBudget(config.scenario, config.drain_budget_bytes), buffer, result.records,
                      result);
            if (!result.error.ok() || std::chrono::steady_clock::now() >= settle_deadline) {
              break;
            }
            // 持续提交查询请求，每个请求身份都不同（幂等只对同一 request_id 生效，重复提交
            // 同一个身份会被当成重放而不再入队），因此队列只增不减。
            ++query_index;
            const ControlRequest query = MakeRequest(
                config.scenario,
                std::string("req-") + mock_profile_scenario_slug(config.scenario) + "-q" +
                    std::to_string(query_index),
                "query", deadline_ms);
            if (EncodeFrame(query, frame)) {
              gateway.feed(connection, frame);
            }
          }
          if (gateway.connection_stats(connection).open) {
            result.error = Error{ErrorCode::kTimeout, "慢消费场景没有在有界队列上触发关闭"};
          }
          if (result.error.ok() && !WaitForSessionSettled(gateway)) {
            result.error = Error{ErrorCode::kTimeout, "会话没有在预算内收敛"};
          }
        } else if (!WaitForSessionSettled(gateway)) {
          result.error = Error{ErrorCode::kTimeout, "会话没有在预算内收敛"};
        }
        if (result.error.ok()) {
          // 取字节直到终态事件到达、连接被关闭或等待预算用尽。慢消费场景在这里只会取到关闭
          // 之后剩下的字节，因此不会因为“再多读一点”而掩盖已经发生的关闭。
          PumpConnection(config, gateway, connection, buffer, result.records, result);
        }
      }

      // 4) 等槽位彻底停止推进，再退出监督器。两步分开是为了让“取消耗时”与“清理耗时”在
      //    证据里可分辨：前者已经在第 2 步单独等过。
      if (!WaitForSlotSettled(supervisor) && result.error.ok()) {
        result.error = Error{ErrorCode::kTimeout, "会话槽位没有在预算内停止推进"};
      }
      const auto shutdown = supervisor.shutdown(kSettleBudget);
      if (!shutdown.ok() && result.error.ok()) {
        result.error = shutdown.error;
      }
      // 线程账目取自监督器的退出状态：只有它报告已退出，本层才有依据说“工作线程已经 join”。
      // 其余情况按可能被 detach 处理，宁可比实际悲观，也不谎报“已经交还”。
      result.ledger.threads_joined =
          supervisor.status().state == SupervisorState::kClosed ? result.ledger.threads_created : 0;
      result.ledger.threads_detached = result.ledger.threads_created - result.ledger.threads_joined;

      // 5) 记录关闭原因并关闭连接。“为什么关的”与“关没关”是两个可以分别核对的字段：
      //    前者证明服务端的有界策略生效，后者证明没有留下打开的连接。
      // 关闭之前再取一次字节：终态事件可能在等待或退出阶段才被投递，若不取走就关闭，
      // 它会随连接一起消失——而"会话以什么终态收敛"正是这条命令要交付的东西。入口不会
      // 清空已关闭连接的待发字节，所以这一步是必须的，不是保险。
      DrainOnce(gateway, connection, EffectiveDrainBudget(config.scenario, config.drain_budget_bytes), buffer, result.records, result);
      const auto stats_before = gateway.connection_stats(connection);
      result.close_reason = nexweave::gateway::to_string(stats_before.close_reason);
      result.observed.connection_closed_by_server =
          IsServerInitiatedClose(stats_before.close_reason);
      result.observed.slow_client_close =
          stats_before.close_reason == GatewayCloseReason::kSlowClient;
      gateway.close_connection(connection);
      result.ledger.connections_open = gateway.status().open_connections;

      // 会话收敛之后从运行记录里取"回答到底产生了多少音频"。这一项必须来自会话自己的账目，
      // 而不是线上事件：v1 的请求入口只交付逐轮文本与终态，逐帧 PCM 下行属于后续传输任务。
      // 记录可能为空（一次都没收敛），此时帧数保持 0，与"确实没出声"一致。
      const auto record = factory.last_run();
      if (record != nullptr) {
        for (const auto& turn : record->result.turns) {
          result.rendered_frames += turn.pcm_frames.size();
        }
      }
      result.observed.audio_rendered = result.rendered_frames > 0;
    }
  }

  // 形态判定：先在**已经交付的记录**上补齐派生项，再与预期比较。补齐集中在这里而不是散落在
  // 读取路径上，是为了让“什么算交付了音频/文本”只有一处定义。
  for (const std::string& record : result.records) {
    if (record.compare(0, kEventPrefixSize, kEventPrefix) != 0) {
      continue;
    }
    const auto decoded = nexweave::protocol::decode_event_metadata(record.substr(kEventPrefixSize));
    if (!decoded.ok()) {
      continue;
    }
    if (decoded.value->type == DataEventType::kToken) {
      result.observed.text_delivered = true;
    }
    if (decoded.value->type == DataEventType::kError &&
        decoded.value->error_code == ErrorCode::kCancelled) {
      result.observed.cancellation_observed = true;
    }
  }
  const MockProfileExpectation expected = mock_profile_expectation(config.scenario);
  result.expectation_matched = CompareExpectation(expected, result.observed, result.mismatch);
  result.exit_code = (result.error.ok() && result.expectation_matched) ? 0 : 1;
  // 汇总与清单在建产物之前生成：它们是产物的内容来源，反过来不行。
  nlohmann::json summary;
  summary["scenario"] = result.scenario_name;
  summary["stream_id"] = config.stream_id;
  summary["config_hash"] = Fingerprint(config_text);
  summary["input_hash"] = Fingerprint(input_text);
  summary["output_configured"] = !config.output_dir.empty();
  summary["exit_code"] = result.exit_code;
  summary["error"] = result.error.ok() ? "none" : "failed";
  summary["error_code"] = result.error.ok() ? "none" : ErrorCodeName(result.error.code);
  summary["terminal_error_code"] =
      result.terminal_error.ok() ? "none" : ErrorCodeName(result.terminal_error.code);
  summary["expectation_matched"] = result.expectation_matched;
  summary["expectation_mismatch"] = result.mismatch;
  summary["responses"] = result.response_count;
  summary["events"] = result.event_count;
  summary["terminal_events"] = result.terminal_event_count;
  summary["token_events"] = result.token_event_count;
  summary["rendered_frames"] = result.rendered_frames;
  summary["cancel_frames"] = result.cancel_frames;
  summary["audio_rendered"] = result.observed.audio_rendered;
  summary["close_reason"] = result.close_reason.empty() ? "none" : result.close_reason;
  summary["connection_closed_by_server"] = result.observed.connection_closed_by_server;
  summary["slow_client_close"] = result.observed.slow_client_close;
  summary["threads_created"] = result.ledger.threads_created;
  summary["threads_joined"] = result.ledger.threads_joined;
  summary["threads_detached"] = result.ledger.threads_detached;
  summary["connections_opened"] = result.ledger.connections_opened;
  summary["connections_open"] = result.ledger.connections_open;
  summary["artifacts_written"] = result.ledger.artifacts_written;
  summary["artifacts_removed"] = result.ledger.artifacts_removed;
  summary["quiesced"] = result.ledger.quiesced();
  result.summary_json = summary.dump();

  // 运行清单复用可观测层已经定义的结构，而不是另造一份：字段集合与后续证据任务一致，
  // 因此这里填过的字段不需要在别处再翻译一次。填不出来的字段留空或写 none——编造一个
  // 看起来像证据的值，比留空更糟。
  nexweave::observability::RunManifest manifest;
  manifest.run_id = std::string("mock-") + result.scenario_name;
  manifest.git_commit = "none";
  manifest.compiler = "none";
  manifest.cmake = "none";
  manifest.runtime = std::string("nexweave-mock-profile/") + result.scenario_name;
  manifest.driver = "none";
  manifest.model = "fake";
  manifest.config_hash = Fingerprint(config_text);
  manifest.input_hash = Fingerprint(input_text);
  manifest.device = "none";
  manifest.profile = "mock";
  // 执行命令必须足以复现这一次运行：只写场景名会漏掉真正改变行为的旋钮与身份，于是
  // 清单里记的命令跑出来是另一次运行。整个命令行在核心逻辑里构造一次，命令行入口按同一
  // 规则回显，两处不会各写一份而将来分歧。
  manifest.command = BuildCommandLine(config, result.scenario_name);
  // 时间戳不参与确定性输出：同一配置必须得到逐字节一致的清单，因此这里不使用"当前时间"，
  // 而是明确的占位值。真实开始/结束时间由后续的运行证据任务在写盘时采集，本层不猜。
  manifest.start_time = "none";
  manifest.end_time = "none";
  const auto encoded_manifest = nexweave::observability::encode_manifest(manifest);
  result.manifest_json =
      encoded_manifest.ok() ? *encoded_manifest.value : std::string("{\"error\":\"manifest\"}");

  // 产物：先写、再删。写入失败按运行级失败报告；删除失败说明“没有残留”不成立，同样升级为
  // 运行级失败——一个留下文件的运行不能被当成干净运行。
  result.ledger.artifacts_written = WriteArtifacts(config, result);
  if (!config.output_dir.empty() && result.ledger.artifacts_written < 3 && result.error.ok()) {
    result.error = Error{ErrorCode::kDeviceFailure, "输出产物写入失败"};
    result.exit_code = 1;
  }
  result.ledger.artifacts_removed = RemoveArtifacts(config);
  if (!result.ledger.artifacts_removed && result.error.ok()) {
    result.error = Error{ErrorCode::kDeviceFailure, "输出产物未能删除"};
    result.exit_code = 1;
  }
  if (!result.ledger.quiesced() && result.error.ok()) {
    result.error = Error{ErrorCode::kDeviceFailure, "运行结束后仍有未交还的资源"};
    result.exit_code = 1;
  }
  // 资源账目最终确定之后重新生成一次汇总：否则 artifacts_removed 与 quiesced 会停留在写入
  // 之前的取值，把一次有残留的运行报告成干净的——那正是这份账目要防止的事。
  summary["exit_code"] = result.exit_code;
  summary["error"] = result.error.ok() ? "none" : "failed";
  summary["error_code"] = result.error.ok() ? "none" : ErrorCodeName(result.error.code);
  summary["artifacts_written"] = result.ledger.artifacts_written;
  summary["artifacts_removed"] = result.ledger.artifacts_removed;
  summary["quiesced"] = result.ledger.quiesced();
  result.summary_json = summary.dump();
  return result;
}

}  // namespace nexweave::app
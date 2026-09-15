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

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "nexweave_version_from_cmake.h"

#include "../backend/fake_asr.hpp"
#include "../backend/fake_audio.hpp"
#include "../backend/fake_llm.hpp"
#include "../backend/fake_rag.hpp"
#include "../backend/fake_tts.hpp"
#include "../domain/audio_frame.hpp"
#include "../domain/error.hpp"
#include "../gateway/gateway.hpp"
#include "../observability/observability.hpp"
#include "../observability/run_evidence.hpp"
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
using nexweave::observability::IPlaybackBoundaryObserver;
using nexweave::observability::MilestoneRecord;
using nexweave::observability::RunEnvironment;
using nexweave::observability::RunEvidenceConfig;
using nexweave::observability::RunEvidenceRecorder;
using nexweave::observability::RunOutcome;
using nexweave::observability::SteadyMonotonicClock;
using nexweave::protocol::ControlRequest;
using nexweave::protocol::ControlResponse;
using nexweave::protocol::DataEventType;
using nexweave::runtime::ISessionCancelTarget;
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
  // 返回 true 仅当本次调用确实在闸门位置被拦下并已放行——也就是"这一帧就是闸门指定
  // 的那一帧"。调用方据此把后续动作绑定在配置的打断点上，而不是绑定在放行之后的第一
  // 帧上（两者在打断点大于 1 时不是同一帧）。返回值只是对已发生事实的报告，不改变状态。
  bool reach(std::size_t written_frames) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (released_ || written_frames < stop_after_frame_) {
      return false;
    }
    hit_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this] { return released_; });
    return true;
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
//
// 设备侧播放边界也在这里报告：会话只知道"我把第一帧交给了播放组件"，只有汇知道
// "帧真的写出去了"。两条事实的线性化点不同，合并它们会掩盖"交付成功但写入失败"。
class PauseSink final : public capability::IAudioSink {
 public:
  PauseSink(FakeAudioSink& inner, PauseGate& gate) : inner_(inner), gate_(gate) {}

  // 挂接设备侧边界观察者（借用指针，可为 nullptr）。它只在**第一帧**成功写出之后
  // 被通知一次；重复挂接以最后一次为准，不分配、不阻塞。
  void set_boundary_observer(IPlaybackBoundaryObserver* observer) { boundary_observer_ = observer; }

  // 设定设备时钟：每成功写出一帧就把逻辑时间推进一个帧长，表示这一帧已经播完。
  // 真实设备的播放进度由硬件时钟推动；确定性夹具没有真实时间，只能由"帧已经写出"
  // 这一事实推动。缺了这一步，会话永远停在"已写出、尚未播完"，每一轮都会以播放未
  // 完成失败收敛——那会让"合成结束"与"播放结束"两个时刻在证据里永远缺失。
  // 传入 nullptr 表示不推进时钟（用于只关心写出行为的夹具）。
  void set_device_clock(nexweave::runtime::IPlaybackClock* clock, std::int64_t frame_ms) {
    device_clock_ = clock;
    frame_ms_ = frame_ms;
  }

  // 接收拥有者交来的取消入口（借用；空函数对象表示入口失效）。它由拥有者在建立会话
  // 时写入、在会话结束时清空，两次都发生在工作线程启动之前或会话结束之后，因此这里
  // 不需要加锁：写入与"会话线程上的读取"之间由线程的建立/join 建立先后关系。
  void set_turn_cancel_entry(std::function<void()> entry) { turn_cancel_ = std::move(entry); }

  // 布置设备侧停止指令：在闸门放行之后、由会话线程请求取消当前轮次。
  // 必须在会话开始之前布置（与 arm 同一处），使"这次运行会不会喊停"成为构造性质。
  void arm_turn_cancel(bool armed) { cancel_after_gate_ = armed; }

  void arm(std::size_t stop_after_frame) { gate_.arm(stop_after_frame); }
  bool wait_until_hit(std::chrono::milliseconds budget) { return gate_.wait_until_hit(budget); }
  void release() { gate_.release(); }

  nexweave::domain::OperationResult open() override { return inner_.open(); }
  nexweave::domain::OperationResult write(const nexweave::domain::AudioFrame& frame) override {
    // 先判断这一帧是不是首帧，再写入：写入之后计数已经加一，那时无法区分"这是第一帧"
    // 与"这至少是第二帧"。
    const bool first_frame = inner_.frames().empty();
    const nexweave::domain::OperationResult written = inner_.write(frame);
    if (written.ok()) {
      // 先推进设备时间再通知边界与闸门：帧写出的那一刻这一帧就开始占用设备时间，
      // 顺序反过来会让"已经播完的帧数"在闸门阻塞期间与设备时间对不上。
      if (device_clock_ != nullptr && frame_ms_ > 0) {
        device_clock_->advance(frame_ms_);
      }
      if (first_frame && boundary_observer_ != nullptr) {
        boundary_observer_->on_first_frame_written();
      }
      // 闸门放行之后再发出设备侧停止指令：本函数的调用栈属于会话线程，因此这正是
      // "播放组件在自己的投递回调里请求取消"这一合法重入点。放在放行之后而不是之前，
      // 是为了让"受理停止那一刻已经写出多少帧"完全由闸门位置决定。一次性置位保证
      // 只会请求一次；即使重复请求，会话层的取消也是幂等的。
      const bool at_gate = gate_.reach(inner_.frames().size());
      if (at_gate && cancel_after_gate_ && !turn_cancel_issued_ && turn_cancel_) {
        turn_cancel_issued_ = true;
        turn_cancel_();
      }
    }
    return written;
  }
  nexweave::domain::OperationResult close() noexcept override { return inner_.close(); }
  nexweave::domain::OperationResult cancel() noexcept override { return inner_.cancel(); }

 private:
  FakeAudioSink& inner_;
  PauseGate& gate_;
  IPlaybackBoundaryObserver* boundary_observer_ = nullptr;
  nexweave::runtime::IPlaybackClock* device_clock_ = nullptr;
  std::int64_t frame_ms_ = 0;
  // 设备侧停止指令的入口与开关。入口由拥有者交接，开关由调用方在会话开始前布置，
  // 两者都只在会话线程上被读取，因此不需要加锁；turn_cancel_issued_ 保证只请求一次。
  std::function<void()> turn_cancel_;
  bool cancel_after_gate_ = false;
  bool turn_cancel_issued_ = false;
};
// 进程内能力夹具。它拥有全部能力对象，并通过引用把它们交给会话工厂；成员声明顺序即构造
// 顺序，也就是依赖顺序（被借用的对象先于借用者建立）。
class MockProfileFixture final : public ISessionCancelTarget {
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
    // 把模拟设备的时钟推进交给音频汇：成员此时已全部构造完成，因此取地址是安全的。
    // 帧长取自领域层的音频契约，不在这里另写一个 20。
    pause_sink_.set_device_clock(
        &clock_, static_cast<std::int64_t>(nexweave::domain::kAudioFrameDurationMs));
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

  // 把设备侧播放边界观察者转交给音频汇包装：本层其余部分不需要知道它的存在。
  void set_playback_boundary_observer(IPlaybackBoundaryObserver* observer) {
    pause_sink_.set_boundary_observer(observer);
  }

  // 布置"会话在闸门放行之后被设备喊停"。（只有取消场景会打开它。）
  void arm_turn_cancel(bool armed) { pause_sink_.arm_turn_cancel(armed); }

  // 接收拥有者交来的取消入口。会话还没建立时收到空函数对象（会话结束），此时设备
  // 已经不会再写帧，因此直接转发即可。
  void set_turn_cancel_entry(std::function<void()> entry) override {
    pause_sink_.set_turn_cancel_entry(std::move(entry));
  }

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
  // 逻辑时钟由音频汇按帧推进（见 PauseSink::set_device_clock）：它不读真实时间，因此
  // 每次运行的时序完全一致；同时它又是会话判定"本轮是否播完"的唯一依据，缺了推进就
  // 没有一轮能成功收敛。
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
  // 产物策略决定这次运行留下什么，因此它是行为的一部分而不是展示细节：不写进指纹，
  // 两次产物不同的运行会得到同一个配置哈希。
  text += "\nartifacts=";
  text += to_string(config.artifact_policy);
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

// 构建工具链的稳定文本名。用整数宏拼装而不是照抄 __VERSION__：后者带大量构建配置
// 细节，同一版编译器在不同打包方式下会产生不同字符串，而证据关心的是“哪一版编译器”。
std::string BuildCompilerName() {
#if defined(__clang__)
  return "clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) +
         "." + std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
  return "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
         std::to_string(__GNUC_PATCHLEVEL__);
#else
  return "unknown";
#endif
}

// 本次运行的机器与版本事实。没有硬件项时写 "none" 而不是编一个看起来像证据的值：
// 这份清单的作用是让人判断“这个结论是在什么环境下得到的”，编造字段会直接破坏它。
// 取不到 Git 提交（从源码压缩包构建）时同样如实写 unknown，也不让构建失败。
RunEnvironment BuildEnvironment(const std::string& scenario_name) {
  RunEnvironment environment;
  const std::string commit = NEXWEAVE_BUILD_GIT_COMMIT;
  const std::string cmake_version = NEXWEAVE_BUILD_CMAKE_VERSION;
  environment.git_commit = commit.empty() ? std::string("unknown") : commit;
  environment.compiler = BuildCompilerName();
  environment.cmake = cmake_version.empty() ? std::string("unknown") : cmake_version;
  environment.runtime = std::string("nexweave-mock-profile/") + scenario_name;
  // 本层只组装进程内确定性夹具：没有 NPU、没有模型文件、没有声卡，也没有传输库。
  environment.driver = "none";
  environment.model = "fake";
  environment.device = "none";
  return environment;
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
// 输出产物的文件名。它们写在配置给出的目录下；目录本身由调用方持有，因此运行结束只处理
// 文件，不处理目录——删除别人的目录不是本层的权限范围。
//
// 五份产物的分工：清单记录“用什么版本、什么配置、什么命令跑的”，事件流是里程碑的
// 逐条原文，指标流是它们的时间与步数投影，摘要把这些投影成给人读的表格，协议报文
// 保留客户端视角的原始交互记录（v1 只有逐轮文本与终态，没有逐帧 PCM 下行）。
constexpr char kManifestFileName[] = "run-manifest.json";
constexpr char kEventsFileName[] = "events.jsonl";
constexpr char kMetricsFileName[] = "metrics.jsonl";
constexpr char kProtocolFileName[] = "protocol.jsonl";
constexpr char kSummaryFileName[] = "summary.md";
// 一次运行应当产生的产物个数。少于它即表示产物不完整，必须按运行级失败报告：
// 缺一份却报告成功，会让读者以为证据齐备。
constexpr std::size_t kArtifactCount = 5;

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
    // 命令行入口按策略回显不同的开关：只写 --out-dir 会让人以为产物不会被保留，
    // 于是照抄清单里的命令跑出来是另一种留存行为。
    command += config.artifact_policy == MockProfileArtifactPolicy::kRetained
                   ? " --evidence-dir "
                   : " --out-dir ";
    command += config.output_dir;
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
//
// 为什么证据文本为空就整体按失败处理：空文件在目录里看起来“写成功了”，而读者打开
// 之后什么也看不到——那比没有文件更容易被误读成“这次运行没有可观察事实”。
std::size_t WriteArtifacts(const MockProfileConfig& config, const MockProfileResult& result) {
  if (config.output_dir.empty()) {
    return 0;
  }
  if (result.manifest_json.empty() || result.events_jsonl.empty() ||
      result.metrics_jsonl.empty() || result.summary_markdown.empty()) {
    return 0;
  }
  if (!EnsureDirectory(config.output_dir)) {
    return 0;
  }
  // 协议报文由入口交出的原始行拼成：每行已经带好 response/event 前缀，本层不再改写，
  // 保留客户端视角的原始顺序。
  std::string protocol;
  for (const std::string& record : result.records) {
    protocol += record;
    protocol.push_back('\n');
  }
  std::size_t written = 0;
  written +=
      WriteFile(JoinPath(config.output_dir, kManifestFileName), result.manifest_json + "\n") ? 1
                                                                                            : 0;
  written += WriteFile(JoinPath(config.output_dir, kEventsFileName), result.events_jsonl) ? 1 : 0;
  written +=
      WriteFile(JoinPath(config.output_dir, kMetricsFileName), result.metrics_jsonl) ? 1 : 0;
  written +=
      WriteFile(JoinPath(config.output_dir, kProtocolFileName), protocol) ? 1 : 0;
  written +=
      WriteFile(JoinPath(config.output_dir, kSummaryFileName), result.summary_markdown) ? 1 : 0;
  return written;
}

// 删除本次运行写入的产物。返回“目录里已经没有本次产物”是否成立。
bool RemoveArtifacts(const MockProfileConfig& config) {
  if (config.output_dir.empty()) {
    return true;
  }
  bool clean = true;
  clean = RemoveFile(JoinPath(config.output_dir, kManifestFileName)) && clean;
  clean = RemoveFile(JoinPath(config.output_dir, kEventsFileName)) && clean;
  clean = RemoveFile(JoinPath(config.output_dir, kMetricsFileName)) && clean;
  clean = RemoveFile(JoinPath(config.output_dir, kProtocolFileName)) && clean;
  clean = RemoveFile(JoinPath(config.output_dir, kSummaryFileName)) && clean;
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

const char* to_string(MockProfileArtifactPolicy policy) noexcept {
  switch (policy) {
    case MockProfileArtifactPolicy::kTransient:
      return "transient";
    case MockProfileArtifactPolicy::kRetained:
      return "retained";
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
  // 策略值必须是自己认得的枚举值：越界值来自强制转换或反序列化错误，落进 else 分支会
  // 悄悄按临时策略运行，把本该留档的证据删掉——那正好与调用方的意图相反。
  if (config.artifact_policy != MockProfileArtifactPolicy::kTransient &&
      config.artifact_policy != MockProfileArtifactPolicy::kRetained) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "未知的产物留存策略");
  }
  // 没有输出目录就没有产物可留：把"要求保留"降级成"什么都不留"是一次静默失败，
  // 调用方会以为证据已经落盘，因此这里按配置错误拒绝。
  if (config.artifact_policy == MockProfileArtifactPolicy::kRetained &&
      config.output_dir.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "保留产物必须给出输出目录");
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

  // 运行证据记录器先于任何资源建立：运行起点必须早于第一个连接与线程，否则“run_start →
  // 首帧”的差值会把建立开销算进链路耗时，而那段开销与场景无关。
  RunEvidenceConfig evidence_config;
  evidence_config.run_id = std::string("mock-") + result.scenario_name;
  evidence_config.profile = "mock";
  // 清单里记的命令必须与命令行入口真正回显的命令是同一条：整个命令行只在这里构造一次，
  // 命令行入口按同一规则拼装，两处不会各写一份而将来分歧。
  evidence_config.command = BuildCommandLine(config, result.scenario_name);
  // 命令哈希与配置/输入哈希取自同一处实现：把命令原文与它的指纹放在一起，读者才能判断
  // “清单里记的命令”和“指纹对应的命令”是不是同一条。
  evidence_config.command_hash = Fingerprint(evidence_config.command);
  evidence_config.config_hash = Fingerprint(config_text);
  evidence_config.input_hash = Fingerprint(input_text);
  evidence_config.request_id = ScenarioRequestId(config.scenario);
  evidence_config.session_id = ScenarioSessionId(config.scenario);
  evidence_config.environment = BuildEnvironment(result.scenario_name);
  // 帧长取自领域层的音频契约，不在这里再写一个 20：抄第二遍就等于埋下“推导用的帧长与
  // 真实帧长不一致”的分歧点。
  evidence_config.frame_ms = static_cast<std::int64_t>(nexweave::domain::kAudioFrameDurationMs);
  SteadyMonotonicClock monotonic_clock;
  RunEvidenceRecorder evidence(evidence_config, monotonic_clock);
  result.artifact_policy_name = to_string(config.artifact_policy);

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

    // 观察者接缝在建立会话之前装好：标记观察者要看到最先发生的里程碑，生成观察者要看到
    // 第一个 token，两者都必须在会话开始执行之前就位，否则最初的事实会永久缺失。
    factory.set_generation_observer(&evidence);
    factory.set_marker_observer(&evidence);
    // 取消入口也必须在此之前注册：拥有者在建立会话时立刻把它交出来，晚一步注册就会
    // 让这一次会话的停止指令没有投递目标。
    factory.set_cancel_target(&fixture);
    // 设备侧边界由音频汇包装报告：它知道“帧真的写出去了”，会话只知道“帧交给播放组件了”。
    fixture.set_playback_boundary_observer(&evidence);

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
        // 取消场景的停止指令由设备发出：控制面取消只让这次会话该退出，在途轮次仍会
        // 跑完，因此只有设备侧的取消才能让会话真的停下并提交取消各阶段。布置同样必须
        // 在提交创建之前完成，理由与闸门相同：会话跑得比任何轮询都快。
        fixture.arm_turn_cancel(true);
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
          DrainOnce(gateway, connection,
                    EffectiveDrainBudget(config.scenario, config.drain_budget_bytes), buffer,
                    result.records, result);
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
            DrainOnce(gateway, connection,
                      EffectiveDrainBudget(config.scenario, config.drain_budget_bytes), buffer,
                      result.records, result);
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
            DrainOnce(gateway, connection,
                      EffectiveDrainBudget(config.scenario, config.drain_budget_bytes), buffer,
                      result.records, result);
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
      DrainOnce(gateway, connection,
                EffectiveDrainBudget(config.scenario, config.drain_budget_bytes), buffer,
                result.records, result);
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

  // 运行证据收尾：结论在退出码确定之后给出，因此 run_end 携带的是这次运行最终的退出码与
  // 错误码，而不是一个在收尾途中就被写死的旧值。产物写入/删除的结果不在这里——它属于
  // 采集侧，写进被采集的内容里就成了“文件记录自己写失败”的循环。
  RunOutcome outcome;
  outcome.exit_code = result.exit_code;
  outcome.error_code = result.error.ok() ? "none" : ErrorCodeName(result.error.code);
  outcome.expectation_matched = result.expectation_matched;
  // 音频帧数取自会话自己的运行记录，而不是线上事件：v1 的请求入口只交付逐轮文本与终态。
  outcome.audio_frames = result.rendered_frames;
  evidence.finish(outcome);
  result.milestones = evidence.milestones();
  result.events_jsonl = evidence.events_jsonl();
  result.metrics_jsonl = evidence.metrics_jsonl();
  result.summary_markdown = evidence.summary_markdown();
  result.overlap_proven = evidence.overlap_proven();
  result.metric_count =
      static_cast<std::size_t>(std::count(result.metrics_jsonl.begin(), result.metrics_jsonl.end(),
                                          '\n'));
  // 运行清单由证据记录器渲染：版本、配置、输入、命令与日历时间来自同一份运行事实，
  // 本层不再自己拼第二份，避免两处字段集合将来分歧。
  result.manifest_json = evidence.manifest_json();

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
  // 证据的确定性投影：条数、策略与因果结论都能逐字节复现，因此可以进汇总；实测时间只进
  // metrics.jsonl 与摘要，不进这里，否则确定性门禁会被实测值破坏。
  summary["artifacts_policy"] = result.artifact_policy_name;
  summary["milestones"] = result.milestones.size();
  summary["metrics"] = result.metric_count;
  summary["overlap_proven"] = result.overlap_proven;
  result.summary_json = summary.dump();

  // 产物：先写，再按策略处理。写入失败按运行级失败报告；采用保留策略时产物是交付物，
  // 采用临时策略时它们必须被删除——留下文件的临时运行不能被当成干净运行。
  result.ledger.artifacts_written = WriteArtifacts(config, result);
  if (!config.output_dir.empty() && result.ledger.artifacts_written < kArtifactCount &&
      result.error.ok()) {
    result.error = Error{ErrorCode::kDeviceFailure, "输出产物写入失败"};
    result.exit_code = 1;
  }
  const bool retained_policy = !config.output_dir.empty() &&
                               config.artifact_policy == MockProfileArtifactPolicy::kRetained;
  if (retained_policy) {
    // 保留策略下文件仍在目录里，但那是本次运行的交付物，不是未交还的资源；账目因此记
    // “有意保留”而不是“删不掉”。写入不完整时保留标志保持假，quiesced() 会如实报未交还。
    result.ledger.artifacts_retained = result.ledger.artifacts_written == kArtifactCount;
    result.ledger.artifacts_removed = false;
  } else {
    result.ledger.artifacts_removed = RemoveArtifacts(config);
    if (!result.ledger.artifacts_removed && result.error.ok()) {
      result.error = Error{ErrorCode::kDeviceFailure, "输出产物未能删除"};
      result.exit_code = 1;
    }
  }
  if (!result.ledger.quiesced() && result.error.ok()) {
    result.error = Error{ErrorCode::kDeviceFailure, "运行结束后仍有未交还的资源"};
    result.exit_code = 1;
  }
  // 资源账目最终确定之后重新生成一次汇总：否则 artifacts_removed / artifacts_retained 与
  // quiesced 会停留在写入之前的取值，把一次有残留的运行报告成干净的——那正是这份账目要
  // 防止的事。运行证据不在这里重渲染：它的 run_end 描述的是会话运行的结论。
  summary["exit_code"] = result.exit_code;
  summary["error"] = result.error.ok() ? "none" : "failed";
  summary["error_code"] = result.error.ok() ? "none" : ErrorCodeName(result.error.code);
  summary["artifacts_written"] = result.ledger.artifacts_written;
  summary["artifacts_removed"] = result.ledger.artifacts_removed;
  summary["artifacts_retained"] = result.ledger.artifacts_retained;
  summary["quiesced"] = result.ledger.quiesced();
  result.summary_json = summary.dump();
  return result;
}

}  // namespace nexweave::app
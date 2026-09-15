// 进程内会话拥有者：把单进程 Session 应用接到 Supervisor 的“会话拥有者”接缝上。
//
// 它解决什么问题
// --------------
// Supervisor 只认三个入口——建立、执行、清理——而真正跑一轮会话的是 SessionApp：它自己
// 按配置建立输入生产者、逐轮驱动会话编排，并在返回前释放全部资源。本文件是这两层之间唯一的
// 胶水，因此刻意保持很薄：不复制任何编排逻辑，也不替会话做决定。
//
// 阶段映射与“就绪”的含义
// ----------------------
//   start()       校验应用配置。它**不**打开输入：输入由 SessionApp::run() 内部按配置建立
//                 并释放，因此这一步表达的是“配置就绪”，不是“设备就绪”。真正的设备就绪
//                 等待属于真实音频适配器，多进程部署下的就绪等待由子进程拥有者负责。
//   run()         调用 SessionApp::run()，阻塞到全部轮次收敛，并把运行结果发布成运行记录。
//   cleanup()     空成功。SessionApp::run() 在返回前已经关闭输入、停止播放并销毁生产者，
//                 因此这里没有需要交还的资源。仍然显式返回成功，是为了让“拥有者确认资源
//                 已经交还”这条不变量在接缝上成立，而不是靠“这个实现碰巧什么都不做”。
//   request_stop() 转发 SessionApp::request_stop()：它只做一次原子置位，满足接缝对
//                  “非阻塞、不分配、不抛异常、可从任意线程调用”的要求。
//
// 资源与线程归属
// --------------
// 本文件不创建线程、文件、socket 或设备句柄。SessionApp 与它借用的能力对象都由调用方注入，
// 生命周期必须长于工厂。三个生命周期入口由 Supervisor 的同一个工作线程串行调用，因此拥有者
// 内部不需要加锁；唯一允许跨线程的是 request_stop()，而它转发到的正是 SessionApp 声明为
// “唯一允许从其他线程调用”的入口。拥有者对象在 create() 的调用线程上构造，会话对象因此在
// 工作线程启动之前就已经固定下来，request_stop() 不会读到半构造的指针。
//
// 运行记录
// --------
// 每次会话收敛后，拥有者把“启动身份 + 应用层验收结果”作为**副本**发布到工厂。控制路径通过
// last_run() 取用，因此不需要访问会话内部状态，也不会延长任何会话对象的寿命。记录只保留
// 最近一次：会话级的事件流与运行证据由后续的请求入口与证据任务负责，本适配器不另开一条
// 上报路径，避免同一件事有两个来源。
//
// 已知限制
// --------
// 模拟常驻输入模式（SessionAppInputMode::kSimulatedResident）的采集只能开始一次，因此注入了
// 常驻输入的工厂只能成功跑完一个会话；需要多轮常驻会话时，调用方必须为每次会话提供新的
// 常驻输入对象。这是 SessionApp 自身的限制，本适配器如实传递，不在这里重试或重置它。
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "../capability/backend.hpp"
#include "../runtime/supervisor.hpp"
#include "resident_audio_input.hpp"
#include "session_app.hpp"
#include "session_runtime.hpp"

namespace nexweave::runtime {

// 一次会话收敛后的运行记录：启动身份（含监督器分配的会话序号）+ 应用层验收结果。
// 结果字段的语义见 SessionAppRunResult；这里只做归属，不做解释。
struct SessionAppRunRecord {
  SupervisorSessionSpec spec;
  SessionAppRunResult result;
};

// 最近一次已收敛会话的记录来源。
//
// 它存在的理由：请求入口要回答“刚才那次会话交付了什么”，但它不应该知道会话是怎样被跑起来
// 的。把这个问题的接口固定下来，进程内应用与后续的多进程适配器就能各自实现同一份契约，
// 而请求入口只依赖接口；测试也可以注入一个不跑真实会话的记录来源。
//
// 阻塞与截止时间：本接口没有任何等待语义——实现必须立即返回，不得等待在途会话、不得睡眠、
// 不得访问设备或网络；调用方（请求入口）因此可以在处理控制请求的路径上安全地调用它。
// 没有“超时”这个概念，因为不存在需要等待的对象。
//
// 线程安全：实现必须允许从任意线程调用；返回的记录是不可变快照，调用方可以长期保留。
class ISessionRunSource {
 public:
  virtual ~ISessionRunSource() = default;

  // 返回最近一次收敛会话的快照；还没有任何会话收敛过时返回空指针。
  // 实现不得阻塞、不得等待在途会话，也不得在返回前复制逐帧音频。
  virtual std::shared_ptr<const SessionAppRunRecord> last_run() const = 0;
};

// 会话取消入口的延迟绑定目标（借用指针）。
//
// 它解决什么问题
// --------------
// 取消的唯一合法重入点是「播放或能力组件在自己的投递回调里」（见 SessionRuntime 的取消
// 语义），而设备对象必须先于会话存在——播放组件借用它，因此设备在构造时拿不到会话。
// 本接口把这个交接点显式化：拥有者在会话建立之后把「请求取消当前轮次」放进来，设备在
// 写帧的回调里调用它，于是取消仍然发生在会话线程上、仍然走同一条统一取消路径。
//
// 时序与所有权：set_turn_cancel_entry 只在拥有者构造（控制线程，工作线程启动之前）与
// 会话结束（工作线程，拥有者析构）时各调用一次，传入空的函数对象表示入口失效。
// 调用方**只能在实际运行会话的那条线程上**调用入口——这正是会话声明的合法重入点；
// 从别的线程调用不在契约内，因为那会让能力对象的 cancel() 与 synthesize() 并发。
//
// 为什么用 std::function 而不是裸指针：入口的接收方（设备）与提供方（拥有者）互不
// 认识对方的类型，函数对象是唯一不需要两侧互相包含头文件的交接形式。交接次数固定为
// 每次会话两次，因此它的分配开销只发生在建立与结束各一次。
class ISessionCancelTarget {
 public:
  virtual ~ISessionCancelTarget() = default;
  // 绑定或解绑本次会话的取消入口。实现必须允许与设备侧调用并发（解绑可能发生在
  // 一次迟到的回调之后），因此内部需要同步；空函数对象表示入口失效。
  virtual void set_turn_cancel_entry(std::function<void()> entry) = 0;
};

// 会话拥有者工厂：按同一份应用配置为每次会话构造一个**全新**的 SessionApp。
//
// 为什么每次新建而不是复用一个应用对象：会话收敛后槽位会被下一个会话复用，而复用同一个
// 应用对象会让上一次的轮次结果、停止请求和生成进度观察者漏进下一次。新建让“每次会话互相
// 独立”成为构造性质，而不是清理是否彻底的问题。
//
// 两类职责为什么放在同一个对象里：它既是拥有者工厂，也是运行记录的存放处。理由是这个
// 对象是唯一“必然比每个会话活得久、又对调用方可见”的东西——拥有者在会话结束时就要消失，
// 没有别的地方能安全地接收它的结果。把记录拆到另一个对象里只会让每个拥有者多拿一个必须
// 与工厂同寿命的引用，而不减少任何耦合。记录只是“这一次会话跑了什么、结果如何”的快照，
// 事件流、时间戳与运行清单不在这个对象的职责范围内。
//
// 借用关系：asr/retriever/router/tts/playback 必须比本对象活得久，retriever 还必须比 router
// 活得久；resident 只在模拟常驻模式下使用，llm 可为空。全部按借用保存，本对象不负责它们的
// 生命周期，也不在析构时释放它们。
class SessionAppOwnerFactory final : public ISessionOwnerFactory, public ISessionRunSource {
 public:
  SessionAppOwnerFactory(SessionAppConfig config, capability::IAsr& asr,
                         capability::IRag& retriever, backend::FakeRagRouter& router,
                         capability::ITts& tts, IAudioPlayback& playback,
                         ResidentAudioInput* resident = nullptr,
                         capability::ILlm* llm = nullptr);

  ~SessionAppOwnerFactory() override;

  SessionAppOwnerFactory(const SessionAppOwnerFactory&) = delete;
  SessionAppOwnerFactory& operator=(const SessionAppOwnerFactory&) = delete;

  // 为一次会话产出全新拥有者。它只保存配置副本与借用引用，不打开输入、不读文件、不创建
  // 线程，因此不会失败；error 保持成功。真正的配置校验发生在拥有者的 start()，
  // 使“参数不对”表现为一次建立失败，而不是在工厂里就吞掉。
  std::shared_ptr<ISessionOwner> create(const SupervisorSessionSpec& spec,
                                        domain::Error& error) override;

  // 最近一次收敛会话的运行记录。返回空指针表示还没有任何会话收敛过。
  // 返回值是**不可变快照**：调用方可以任意保留，它不会被后续会话改写；也不需要复制，
  // 因此这条控制路径的开销与会话产出了多少 PCM 无关。
  std::shared_ptr<const SessionAppRunRecord> last_run() const override;

  // 注册取消入口的接收方（借用指针，可为 nullptr）。必须在建立会话之前设置：拥有者
  // 在构造新会话时就把入口交出去，因此交接严格早于工作线程启动。接收方必须比本工厂
  // 活得久；它不参与任何其他判定。
  void set_cancel_target(ISessionCancelTarget* target) noexcept;

  // 为之后建立的每个会话挂接生成进度观察者（借用指针，可为 nullptr）。它必须在建立
  // 会话之前设置：拥有者在构造时就把观察者装到新会话上，因此工作线程看到的始终是已经
  // 固定下来的指针，不存在“注册到一半就开始执行”的窗口。观察者必须比本工厂活得久。
  void set_generation_observer(capability::IGenerationObserver* observer) noexcept;

  // 为之后建立的每个会话挂接活动标记观察者（借用指针，可为 nullptr）。时机与生命周期
  // 要求同生成观察者；两者互相独立，可以只挂其中一个。
  void set_marker_observer(IMarkerObserver* observer) noexcept;

 private:
  // 一次会话的拥有者。定义在实现文件里：它只有生命周期入口，不构成对外契约。
  class Owner;

  // 发布一次会话的运行记录（由工作线程调用）。只保留最近一次，覆盖旧记录。
  void publish(const SessionAppRunRecord& record);

  SessionAppConfig config_;
  capability::IAsr& asr_;
  capability::IRag& retriever_;
  backend::FakeRagRouter& router_;
  capability::ITts& tts_;
  IAudioPlayback& playback_;
  ResidentAudioInput* resident_ = nullptr;
  capability::ILlm* llm_ = nullptr;
  // 取消入口的接收方（借用）。它在建立会话时被读取一次，因此不需要加锁：建立发生在
  // 控制线程上，且严格早于工作线程启动。
  ISessionCancelTarget* cancel_target_ = nullptr;
  // 两个观察者接缝都是借用指针：工厂不拥有它们，也不在析构时释放；它们只在建立会话
  // 的那一刻被读取一次，因此不需要加锁——建立会话发生在控制线程上，且严格早于工作
  // 线程启动。
  capability::IGenerationObserver* generation_observer_ = nullptr;
  IMarkerObserver* marker_observer_ = nullptr;

  mutable std::mutex mutex_;
  // 最近一次收敛记录。用共享指针承载，使 last_run() 只需在锁内取一份引用，不必持锁复制
  // 一份含逐帧 PCM 的结果。
  std::shared_ptr<const SessionAppRunRecord> last_run_;
};

}  // namespace nexweave::runtime

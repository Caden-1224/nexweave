#include "session_app_owner.hpp"

#include <utility>

namespace nexweave::runtime {

// 一次会话的拥有者。生命周期入口的调用顺序由监督器保证：
//   构造（控制线程） → start() → [仅当 start() 成功] run() → cleanup() → 析构（工作线程）
// 因此除 request_stop() 之外的全部状态都只被工作线程触碰，不需要互斥量。
class SessionAppOwnerFactory::Owner final : public ISessionOwner {
 public:
  Owner(SessionAppOwnerFactory& factory, const SupervisorSessionSpec& spec)
      : factory_(factory), spec_(spec) {
    // 会话对象在控制线程上一次性构造完成，因此工作线程与 request_stop() 看到的是同一个
    // 已固定的指针；构造本身不打开输入、不读文件、不创建线程，不会失败。
    app_ = std::make_unique<SessionApp>(factory_.config_, factory_.asr_, factory_.retriever_,
                                        factory_.router_, factory_.tts_, factory_.playback_,
                                        factory_.resident_, factory_.llm_);
    // 观察者在本构造函数里一次装好：此后 app_ 不再变化，工作线程与 request_stop() 看到
    // 的是同一个已固定的会话，不会读到尚未挂接完成的观察者。
    app_->set_generation_observer(factory_.generation_observer_);
    app_->set_marker_observer(factory_.marker_observer_);
    // 把「请求取消当前轮次」交给注册过的接收方。捕获 app_ 的裸指针是安全的：入口
    // 只在工作线程上、由设备回调在会话运行期间调用，而本对象的析构会在会话结束之后
    // 先把入口清空，因此不会留下指向已析构会话的调用路径。
    if (factory_.cancel_target_ != nullptr) {
      factory_.cancel_target_->set_turn_cancel_entry([this]() { app_->cancel_turn(); });
    }
  }

  ~Owner() override {
    // 会话已经结束：先摘掉入口再销毁会话，使设备即使拿到旧入口也打不到已析构的对象。
    // 顺序不能反过来——那样在两次调用之间会存在一段可被设备回调命中的悬空窗口。
    if (factory_.cancel_target_ != nullptr) {
      factory_.cancel_target_->set_turn_cancel_entry(nullptr);
    }
  }

  // 建立：校验应用配置。配置非法时返回 kInvalidInput，使“参数不对”变成一次明确的建立失败，
  // 而不是一个已经开始、又在中途失败的会话；失败后监督器仍会调用 cleanup()，这里无事可做。
  domain::OperationResult start() override {
    return validate_app_config(factory_.config_);
  }

  domain::OperationResult run() override {
    const SessionAppRunResult result = app_->run();
    // 副作用是刻意的：run() 除了执行会话，还要把“这一次跑出了什么”交给工厂，否则调用方
    // 只能看到监督器的计数，看不到会话本身的终态与证据。
    factory_.publish(SessionAppRunRecord{spec_, result});
    // 只把**运行级**错误当作拥有者的执行失败：单轮失败属于那一轮，常驻输入下后续说话仍然
    // 可用（见 SessionAppRunResult 的说明），把它升级成会话失败会掩盖“被打断的一轮”与
    // “整个会话跑不下去”的区别。
    return domain::OperationResult{result.error};
  }

  // 清理：SessionApp::run() 返回前已经关闭输入、停止播放并销毁生产者，本对象没有额外资源
  // 需要交还。显式返回成功，使“拥有者确认资源已交还”在接缝上始终成立。
  domain::OperationResult cleanup() noexcept override {
    return domain::OperationResult::success();
  }

  void request_stop() noexcept override {
    // app_ 在构造时固定，因此这里不需要加锁；转发到的入口本身只做一次原子置位。
    app_->request_stop();
  }

 private:
  SessionAppOwnerFactory& factory_;
  SupervisorSessionSpec spec_;
  std::unique_ptr<SessionApp> app_;
};

SessionAppOwnerFactory::SessionAppOwnerFactory(SessionAppConfig config, capability::IAsr& asr,
                                               capability::IRag& retriever,
                                               backend::FakeRagRouter& router,
                                               capability::ITts& tts,
                                               IAudioPlayback& playback,
                                               ResidentAudioInput* resident, capability::ILlm* llm)
    : config_(std::move(config)),
      asr_(asr),
      retriever_(retriever),
      router_(router),
      tts_(tts),
      playback_(playback),
      resident_(resident),
      llm_(llm) {}

SessionAppOwnerFactory::~SessionAppOwnerFactory() = default;

void SessionAppOwnerFactory::set_generation_observer(
    capability::IGenerationObserver* observer) noexcept {
  generation_observer_ = observer;
}

void SessionAppOwnerFactory::set_marker_observer(IMarkerObserver* observer) noexcept {
  marker_observer_ = observer;
}

void SessionAppOwnerFactory::set_cancel_target(ISessionCancelTarget* target) noexcept {
  cancel_target_ = target;
}

std::shared_ptr<ISessionOwner> SessionAppOwnerFactory::create(const SupervisorSessionSpec& spec,
                                                              domain::Error& error) {
  error = domain::Error{};
  return std::make_shared<Owner>(*this, spec);
}

std::shared_ptr<const SessionAppRunRecord> SessionAppOwnerFactory::last_run() const {
  // 只取一份引用，不复制记录：记录里带着逐帧 PCM，“查询最近一次结果”不应该随会话产出了
  // 多少音频而变贵。记录一旦发布就不再改写，因此调用方拿到的是一个稳定快照。
  const std::lock_guard<std::mutex> guard(mutex_);
  return last_run_;
}

void SessionAppOwnerFactory::publish(const SessionAppRunRecord& record) {
  // 先在工作线程上把记录构造完再进锁：与 last_run() 对称，避免持锁做含逐帧 PCM 的拷贝。
  auto snapshot = std::make_shared<SessionAppRunRecord>(record);
  const std::lock_guard<std::mutex> guard(mutex_);
  last_run_ = std::move(snapshot);
}

}  // namespace nexweave::runtime

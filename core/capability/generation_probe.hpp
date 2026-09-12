// 生成进度接缝：把“token 已交付”“生成已完成”“生成已失败”暴露出来，使同步验证的
// 生成后端也能被外部按事件先后关系检查。
//
// 存在理由：Mock 阶段的后端在 generate() 返回前就把全部 token 交付完，“某一段文本已经
// 进入播放”与“生成还没结束”都发生在这个调用的内部。若没有可观测的线性化点，只能靠
// 墙钟或睡眠去猜，而这两者都不属于确定性证据。
//
// 两个角色分开，是因为它们的绑定时机不同：
//   - IGenerationObserver 是生成后端的观察者，由后端的拥有者（测试或运行夹具）在轮次
//     之间挂接，因此可以晚于后端构造；
//   - IGenerationProbe 是编排层的观察者，实现方通常是业务层自己（例如会话），由编排层
//     在生成开始前挂到实现了 IGenerationProbe 的适配器上。
// 同时实现两者的适配器把事实从后者转给前者，于是两端的绑定都可以发生在构造之后。
//
// 归属与并发：两种指针都是借用，生命周期必须覆盖“挂上到摘掉”这一段；全部回调在交付
// 事件的调用线程上同步执行且严格串行；实现必须非阻塞、不睡眠、不抛异常，也不得回调进
// 正在执行的后端。未挂接时后端不得产生任何额外分支或开销。
#pragma once

#include <string>

namespace nexweave::capability {

// 生成后端的观察者：由后端在生成期间同步报告事实。
class IGenerationObserver {
 public:
  virtual ~IGenerationObserver() = default;

  // 一次生成真正开始（已通过取消与输入校验）时调用一次；用于隔离上一轮的迟到通知。
  virtual void on_generation_started() {}
  // 一个 token 已经交付给接收方之后调用；该 token 的后续处理可能仍在进行。
  virtual void on_token_delivered(const std::string& /*token*/) {}
  // 全部 token 与其后的完成事件都已交付、生成本身即将返回成功时调用。
  virtual void on_generation_completed() {}
  // 生成以失败收敛时调用（取消、回调异常、输入非法等）；message 仅用于诊断。
  virtual void on_generation_failed(const std::string& /*message*/) {}
  // 本次生成是否已经以失败收敛。为真时后端不再报告任何后续事件，避免“失败之后又完成”
  // 这种自相矛盾的证据；观察者只需在 on_generation_failed 里置位。
  virtual bool failed() const noexcept {
    return false;
  }
};

// 编排层的观察者接缝：适配器同时实现它时，编排层可以在生成开始前把自己挂上去。
class IGenerationProbe {
 public:
  virtual ~IGenerationProbe() = default;

  // 一次生成真正开始（已通过取消与输入校验）时调用一次。
  virtual void on_generation_started() {}
  // 一个 token 已经交付给接收方之后调用。
  virtual void on_token_delivered(const std::string& /*token*/) {}
  // 全部 token 与其后的完成事件都已交付、生成本身即将返回成功时调用。
  virtual void on_generation_completed() {}
  // 生成以失败收敛时调用；message 仅用于诊断，机器判定一律使用后端返回的错误码。
  virtual void on_generation_failed(const std::string& /*message*/) {}
};

}  // namespace nexweave::capability

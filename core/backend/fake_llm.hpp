// 确定性 Fake LLM（Large Language Model，大语言模型）适配器声明。
//
// 职责：为 C++17 Mock 阶段提供“非空 prompt → 有序 token → 一次 done”的同步文字生成，
// 用来验证生成事件的顺序、取消封锁、回调失败清理，以及 Session 侧“生成未结束就开始
// 合成与播放”的重叠编排。它不做任何模型推理，也不声称文字质量或生成速度。
#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include "../capability/backend.hpp"
#include "../capability/generation_probe.hpp"

namespace nexweave::backend {

/**
 * FakeLlm 把非空 prompt 映射为预设 token 序列：先按顺序逐项发布 kToken，再发布一次
 * 空文本 kDone，全部在 generate() 返回前同步完成。它不读 prompt 内容，因此“同一
 * prompt 得到同一回答”是夹具性质，不是模型行为；不同 prompt 不会得到不同结果。
 *
 * 夹具约束（都在 set_callback 时校验）：列表非空且每一项非空。空列表或空项返回
 * kInvalidInput，不发布任何事件，也不解除既有的取消封锁。
 *
 * 生命周期与取消：set_callback 是显式的新轮次边界——替换回调、清除取消封锁；
 * cancel() 置位取消封锁。取消的线性化点是“每个 token 交付前”的检查，因此取消后
 * 不再开始交付任何 token；已进入回调的那一个 token 至多完成本次交付，无法撤回。
 * 取消后 generate 返回 kCancelled，只有成功注册回调才能开启下一轮，避免重试造成重复
 * 交付。回调抛出异常时封锁本轮并原样向上传播，接收方自行清理可能已收到的 token。
 *
 * 并发：与 FakeTts 同一约定——set_callback 与 generate 由调用方串行调用；唯一允许并发
 * 的是 cancel()，它只做一次原子置位，因此另一线程在 generate 执行中取消不会破坏内部
 * 一致性。并发取消的精确生效点仍是“下一个 token 交付前”的检查，不是立即打断。
 *
 * 顺序与不可重入：token 严格按夹具顺序交付且恰好一次，done 恰好一次且在所有 token
 * 之后。回调在 generate 的调用线程同步执行，回调期间引用有效；回调不得重入本对象的
 * 任何方法（cancel 除外），也不得销毁本对象。
 *
 * 进度观测：本类同时实现 IGenerationProbe 的注册端——注册后，generate 在“开始、
 * 每个 token 交付后、正常结束、失败”四处同步报告事实，使同步夹具也能被外部按事件
 * 先后关系检查。探针是借用指针，必须比本对象活得久；未注册时不产生任何额外分支。
 *
 * 资源：只拥有夹具、回调与探针的借用指针，不创建线程、文件、进程、socket 或模型句柄，
 * 无内部 deadline、无隐藏队列。分配异常在交付前发生时保持夹具不变，向上传播，由调用方
 * 决定是否重试；析构不需要等待任何后台工作。
 */
class FakeLlm final : public capability::ILlm, public capability::IGenerationProbe {
 public:
  // 按值接收 token 夹具并移动接管；不在构造期校验内容，空列表/空项延迟到
  // set_callback 返回 kInvalidInput，避免构造出“看似可用但永远不会成功”的对象。
  explicit FakeLlm(std::vector<std::string> tokens);

  // 非空回调、非空夹具且无空项才成功；成功即新轮次，替换旧回调并清除取消封锁。
  // 失败保留旧回调与取消状态，因此不会因为一次坏注册而意外恢复已取消的轮次。
  domain::OperationResult set_callback(capability::TextEventCallback callback) override;

  // 先查取消、再查回调与 prompt，分别返回 kCancelled 与 kInvalidInput；失败不交付任何
  // 事件。成功时同步交付 token* 与一次 done 后返回 success，返回后不存在在途回调或
  // 迟到事件。取消在某个 token 交付前到达即从该 token 起停止交付。
  domain::OperationResult generate(const std::string& prompt) override;

  // 幂等、无分配、不抛异常；置位取消封锁即为串行调用下的线性化点。返回后不再有新
  // token 开始交付；已交付的 token 归接收方所有，取消不撤回它们。
  domain::OperationResult cancel() noexcept override;

  // 挂接进度观察者（借用指针，必须比本对象活得久）；传 nullptr 关闭后端侧观测。
  // 观察者可以晚于本对象构造，这是它存在的理由：测试先建夹具、再建探针。
  void set_observer(capability::IGenerationObserver* observer) noexcept;

  // IGenerationProbe 的接收端：编排层把自己挂上来后，后端把事实转给观察者（若已挂接）。
  void set_progress_probe(capability::IGenerationProbe* probe) noexcept override;

 private:
  // 回调路径上的最小封装，保证“已交付”“已完成”“已失败”只在真实发生时各报一次，
  // 且顺序恒为“先交付事实、后通知观察者”。两者都为空时是空操作，不产生额外分支。
  void report_token(const std::string& token);
  void report_completed();
  // 把通知同时送到观察者与编排层探针；两者都可以为空。失败收敛后不再报告后续事件，
  // 避免出现“报告了失败、又报告了完成”的自相矛盾证据。
  void notify_token(const std::string& token);
  void notify_completed();
  void notify_failed(const std::string& message);
  bool reporting_suppressed() const noexcept;

  std::vector<std::string> tokens_;  // 构造后不修改，自有文本不引用调用方容器。
  capability::TextEventCallback callback_;  // 自有函数对象；捕获引用由调用方管理。
  capability::IGenerationObserver* observer_ = nullptr;  // 借用；后端侧观察者，可为空。
  capability::IGenerationProbe* probe_ = nullptr;  // 借用；编排层探针，可为空。
  // 唯一允许并发访问的成员：cancel() 可在另一线程置位，交付路径只做原子读取。
  std::atomic<bool> cancelled_{false};
};

}  // namespace nexweave::backend

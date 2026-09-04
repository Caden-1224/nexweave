// NexWeave Session 生命周期状态机契约。
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "error.hpp"
namespace nexweave::runtime {
/**
 * 约束 Session 阶段迁移并保留可审计轨迹。输入必须是当前阶段允许的事件；成功后
 * 状态与轨迹同步更新，非法事件返回 kInvalidInput 且不改变任何状态。对象不创建
 * 线程、文件、socket 或设备句柄，也不执行 deadline 等待；调用方拥有生命周期。
 * 所有方法以互斥量线性化，trace() 返回独立快照。工作态取消统一进入 Cancelling，
 * 只有 cancel_complete 能回到 Idle；取消清理失败由上层映射为错误并最终驱动收敛。
 * C++17 下未知枚举值按非法输入处理。generation 与状态迁移使用同一把锁线性化：
 * 新请求和有效取消各推进一代，旧代事件在提交前被拒绝。
 */
class SessionStateMachine final {
 public:
  enum class State { kIdle, kListening, kRouting, kThinking, kSpeaking, kCancelling };
  enum class Event { kAudioStart, kAsrFinal, kRouteL0L1, kRouteL2L3, kLlmDone, kTtsDone, kCancel, kCancelComplete };
  // 在互斥量保护下提交使用当前代际的事件；若并发调用已推进代际，本入口可能返回
  // kInvalidInput，调用方应改用显式快照重试；不阻塞外部资源，失败保留状态和轨迹。
  domain::OperationResult dispatch(Event event);
  // generation 是调用方在异步工作开始时捕获的快照；输入必须等于当前代际。
  // 成功后至多迁移一次状态并追加一条轨迹，过时代际返回 kInvalidInput 且无副作用；
  // 方法不等待外部资源，互斥量在返回前释放，适用于回调线程并发调用。
  domain::OperationResult dispatch(Event event, std::uint64_t generation);
  State state() const;
  // 返回当前代际快照；调用方应在启动异步能力后保存它并原样传回 dispatch。
  // 读取受互斥量保护且不阻塞外部资源，生命周期由 SessionStateMachine 拥有。
  std::uint64_t generation() const;
  const char* state_name() const;
  std::vector<std::string> trace() const;
  // 清理状态与轨迹但保留 generation 水位，防止 reset 后旧代事件重新获得合法性；
  // 幂等，不涉及线程、句柄或文件释放。
  void reset();
 private:
  State state_ = State::kIdle;
  std::uint64_t generation_ = 0;
  mutable std::mutex mutex_;
  std::vector<std::string> trace_;
};
}  // namespace nexweave::runtime

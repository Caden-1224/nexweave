// NexWeave Session 生命周期状态机契约。
#pragma once
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
 * C++17 下未知枚举值按非法输入处理，状态机本身不负责 generation 递增或事件过滤。
 */
class SessionStateMachine final {
 public:
  enum class State { kIdle, kListening, kRouting, kThinking, kSpeaking, kCancelling };
  enum class Event { kAudioStart, kAsrFinal, kRouteL0L1, kRouteL2L3, kLlmDone, kTtsDone, kCancel, kCancelComplete };
  // 在互斥量保护下原子提交事件；不阻塞外部资源，失败时保留原状态和轨迹。
  domain::OperationResult dispatch(Event event);
  State state() const;
  const char* state_name() const;
  std::vector<std::string> trace() const;
  // 清理状态与轨迹；幂等，不涉及线程、句柄或文件释放。
  void reset();
 private:
  State state_ = State::kIdle;
  mutable std::mutex mutex_;
  std::vector<std::string> trace_;
};
}  // namespace nexweave::runtime

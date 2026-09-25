// RKLLM 文本推理适配器核心。
//
// 本文件把 ILlm 契约落到“供应方异步回调 + 有界队列 + 调用线程泵”的实现上：
//   - set_callback() 开启新轮，清空旧队列并解除取消封锁；
//   - generate(prompt) 在调用线程阻塞到 Finished/Error，期间把 vendor 回调投递的
//     文本片段交给注册的 ILlm 回调；慢消费者因此阻塞 generate 调用线程，不会阻塞
//     供应方内部回调线程；
//   - cancel() 只置原子标志并尽力 abort 供应方；泵在每个 token 和终态前检查标志，
//     标志置位后不再开始新回调，已交付 token 不撤回；
//   - 队列满、回调异常、供应方错误和供应方停止超时都收敛为结构化错误，并保留
//     “已交付部分输出”的事实。
//
// 线程约定：set_callback/generate/set_progress_probe 由同一调用方串行调用；cancel()
// 允许从其他线程或 token 回调内调用，必须非阻塞。generate() 返回后不再有属于本轮
// 的回调；供应方若在 abort 后仍不停止，核心把 runtime 标记为不可用或交给 close
// 的有限等待处理，不会在仍被 SDK 使用时销毁句柄。
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>

#include "../../capability/backend.hpp"
#include "../../capability/generation_probe.hpp"
#include "rkllm_runtime.hpp"

namespace nexweave::backend {
namespace rkllm_detail {

// 适配器核心边界。这里的每个上限都会在 create 时校验，不用无限队列或无限等待兜底。
struct RkllmStreamConfig {
  // 单个 prompt 的最大 UTF-8 字节数；超过返回 kInvalidInput。
  std::size_t max_prompt_bytes = 16U * 1024U;
  // 单轮累计输出文本上限；超过返回 kBackendFailure 并请求停止供应方。
  std::size_t max_output_bytes = 1024U * 1024U;
  // 供应方回调队列最多容纳多少个待投递片段；满队列不再阻塞 vendor 线程。
  std::size_t max_pending_tokens = 256U;
  // 单个 generate 从启动到终止的最长等待；到点先 abort，再在 stop_wait 内确认停止。
  std::chrono::milliseconds generation_wait{120000};
  // 取消或失败后等待供应方确认停止的时间。
  std::chrono::milliseconds stop_wait{5000};
};

}  // namespace rkllm_detail

// 文本适配器核心。不包含 RKLLM 类型；供应方差异由注入的 IRkllmRuntime 承担。
// 对象拥有 runtime、回调副本、队列和取消标志，析构时先请求停止再尝试关闭 runtime。
class RkllmTextCore final : public capability::ILlm,
                            public capability::IGenerationProbe {
 public:
  // runtime 为空或配置非法时返回 kInvalidInput。成功后对象处于空闲；仍需
  // set_callback 才能 generate。
  static domain::Result<std::unique_ptr<RkllmTextCore>> create(
      std::unique_ptr<rkllm_detail::IRkllmRuntime> runtime,
      rkllm_detail::RkllmStreamConfig config = {});

  RkllmTextCore(const RkllmTextCore&) = delete;
  RkllmTextCore& operator=(const RkllmTextCore&) = delete;
  ~RkllmTextCore() override;

  // 空回调、正在生成或 runtime 已不可用时返回结构化失败。成功即新轮边界：
  // 清空旧队列、重置输出上限和取消标志，但不释放 vendor 模型。
  domain::OperationResult set_callback(capability::TextEventCallback callback) override;

  // 取消优先于输入校验；空回调/空 prompt/超长 prompt 返回 kInvalidInput。
  // 正常完成发 token* 与一次 done；供应方错误发一次 kError 并返回失败；
  // 取消返回 kCancelled；队列满、总输出超限或回调异常返回结构化失败。
  // 返回后本轮回调全部结束；若供应方停止超时，对象会进入明确不可用状态。
  domain::OperationResult generate(const std::string& prompt) override;

  // 幂等、无分配、不抛异常；置原子标志并唤醒泵，尽力 abort 供应方。允许在
  // token 回调内重入，这是 ILlm 契约对取消的唯一例外。
  domain::OperationResult cancel() noexcept override;

  // 挂接编排层探针；借用指针，必须比本对象活得久。核心在其中报告真实发生的
  // started/token/completed/failed 事实；传 nullptr 摘除。
  void set_progress_probe(capability::IGenerationProbe* probe) noexcept override;

 private:
  struct Impl;
  explicit RkllmTextCore(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::backend

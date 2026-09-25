// RKLLM 供应商运行库的窄接缝。
//
// 文本适配器核心只依赖本接口：它把 vendor 回调事实搬到有界队列，并在 generate()
// 调用线程上泵出统一事件。具体实现负责 rkllm_init / rkllm_run_async / rkllm_abort /
// rkllm_destroy，以及 vendor 回调来自哪个线程、何时把模型句柄视为已停止。
//
// 所有权：核心拥有 runtime 对象，runtime 拥有模型句柄与供应方回调注册。
// 生命周期：start_async 成功返回后，runtime 必须只在当前 generation 内回调；
// 核心在取消或超时后调用 abort/wait，确认停止后才会销毁对象。
#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "../../capability/backend.hpp"

namespace nexweave::backend::rkllm_detail {

// 供应商回调状态。Waiting 表示厂商正在等待完整 UTF-8 字符，文本可能只覆盖半个字符；
// 核心会把它暂存并与后续 Normal 文本合并。Finished/Error 是本次生成的终止状态。
enum class VendorCallState {
  kNormal,
  kWaiting,
  kFinished,
  kError
};

struct VendorCallResult {
  VendorCallState state = VendorCallState::kNormal;
  // Normal/Waiting 时是厂商给出的文本片段；Finished/Error 时可为空或诊断文本。
  std::string text;
};

// 一次异步生成的供应商回调。runtime 可以在内部线程调用它，但实现必须保证：
//   - 只复制少量文本并尽快返回，不等待核心消费者；
//   - Finished/Error 恰好出现一次，之后不再回调；
//   - 与 abort()/close() 并发时仍不访问已释放的 runtime 或句柄。
using VendorCallback = std::function<void(VendorCallResult)>;

class IRkllmRuntime {
 public:
  virtual ~IRkllmRuntime() = default;

  // 启动一次异步生成。成功返回后 runtime 拥有本次回调，直到 Finished/Error 或
  // abort+wait 完成为止。失败返回结构化错误且不得再调用 callback。
  virtual domain::OperationResult start_async(const std::string& prompt,
                                              VendorCallback callback) = 0;

  // 尽力请求供应方停止；非阻塞、不抛异常，返回后不保证已经停止。
  virtual void abort() noexcept = 0;

  // 当前是否仍有一轮供应方生成未收到终止状态。
  virtual bool is_running() const noexcept = 0;

  // 等待当前生成停止；返回 true 表示已看到 Finished/Error 或 abort 生效。
  virtual bool wait_until_stopped(std::chrono::milliseconds timeout) = 0;

  // 关闭 runtime：先 abort，再在 wait 内等待停止；只有确认停止才销毁模型句柄。
  // 返回 true 表示已释放句柄并进入不可再用状态；false 表示停止超时，句柄被保留
  // 但对象将不再被使用，由进程隔离或最终退出负责回收。
  virtual bool close(std::chrono::milliseconds wait) noexcept = 0;
};

}  // namespace nexweave::backend::rkllm_detail

// 确定性 Fake RAG（Retrieval-Augmented Generation，检索增强生成）及 L0-L3 路由。
// 只拥有内存夹具，不创建线程、文件、socket 或模型句柄；用于 WSL/Mock 回归。
// 分数是夹具排序量，不是概率；真实知识库和校准由后续适配器负责。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "../capability/backend.hpp"

namespace nexweave::backend {

// L0 是控制意图，L1 是知识直答，L2 是携带命中上下文的推理，L3 是普通对话。
// 枚举值只表达语义，不代表优先级；v1 新增级别必须保持旧调用方的显式分支可编译。
enum class RagRouteLevel {
  kL0,
  kL1,
  kL2,
  kL3
};

// 路由结果拥有检索文本副本和解释字段。L0 的 control_action 非空，L1 的
// direct_answer 使用首个命中；L2/L3 由上层决定是否把 hits 交给 LLM。
struct RagRouteDecision {
  RagRouteLevel level = RagRouteLevel::kL3;
  std::vector<capability::RetrievedChunk> hits;
  std::string reason;
  std::string control_action;
  std::string direct_answer;
};

// Fake 检索夹具拥有全部输入记录。构造时空 id/text 或非有限、负分数抛 invalid_argument；
// retrieve 不阻塞、不修改夹具。空或全空白 query 返回 kInvalidInput，top_k=0 返回空成功，
// 无命中也是成功；结果按分数降序、同分按 id 字典序返回，调用方拥有结果副本。
class FakeRag final : public capability::IRag {
 public:
  explicit FakeRag(std::vector<capability::RetrievedChunk> chunks);
  // 输入必须是非空白 query；top_k=0 是合法空结果。成功返回最多 top_k 个副本，
  // 失败不产生可消费值；同步调用且线程安全责任在调用方，v1 不自动截断输入。
  domain::Result<std::vector<capability::RetrievedChunk>> retrieve(
      const std::string& query, std::size_t top_k) override;

 private:
  std::vector<capability::RetrievedChunk> chunks_;
};

// 路由器同步识别 L0、调用 IRag 并选择阈值分支。retriever 由调用方拥有且必须长于路由器；
// 不创建外部资源，默认串行调用。cancel 后所有 route 返回 kCancelled，reset 开启新轮次。
class FakeRagRouter final {
 public:
  struct Config {
    // direct_answer_threshold >= context_threshold，有限且非负；达到边界即进入对应级别。
    double direct_answer_threshold = 0.90;
    double context_threshold = 0.50;
    // 必须 >0；每次 route 传给 IRag，避免无限制返回夹具。
    std::size_t top_k = 3;
  };

  // 使用默认阈值构造；retriever 生命周期由调用方保证，异常只来自对象构造失败。
  explicit FakeRagRouter(capability::IRag& retriever);
  // 配置非法时抛 invalid_argument；成功后对象处于可路由、未取消状态。
  FakeRagRouter(capability::IRag& retriever, Config config);
  // 非空白 query 才合法；同步返回命中、级别和原因。取消后返回 kCancelled，检索失败
  // 原样传播；不创建线程、不等待 deadline，route/reset/cancel 默认要求外部串行。
  domain::Result<RagRouteDecision> route(const std::string& query);
  // 幂等封锁当前轮次；不撤回已返回的值，也不创建清理资源。
  domain::OperationResult cancel() noexcept;
  // 幂等开启下一轮并清除取消封锁；夹具不重置，重复输入仍得到相同结果。
  domain::OperationResult reset() noexcept;

 private:
  capability::IRag& retriever_;
  Config config_;
  bool cancelled_ = false;
};

}  // namespace nexweave::backend

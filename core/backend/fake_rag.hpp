// 确定性 RAG（Retrieval-Augmented Generation，检索增强生成）夹具与 L0-L3 路由。
//
// FakeRag 只拥有内存排序量，不读知识库文件；Bm25Rag 等真实 IRag 后端通过相同接口接入。
// 路由对后端没有“Fake”假设：它只解释 IRag 返回的分值和顺序，因此 Session 不需要知道
// 检索来自固定夹具还是本地词项索引。
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
// hits 的顺序就是后端决定的证据顺序；路由只会在 L2 按上下文预算裁剪它，不重排。
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
    // L2 注入 LLM 的命中文本 UTF-8 字节预算，必须 >0。预算只统计 hit.text，不包含
    // 上层提示词版式；路由器按分数顺序装入能完整容纳的命中，装不下就不把该条交给模型，
    // 从而避免空上下文或越界上下文。该值必须与知识库和阈值一起校准。
    std::size_t max_context_bytes = 2048;
    // L0 控制词。规范化后的 query 必须与规范化后的控制词完全相等才触发，避免
    // “取消后旧数据怎么处理”这类包含控制词但属于知识问题的查询被误判为控制意图。
    // 每个控制词规范化后不得为空。
    std::vector<std::string> l0_keywords = {"停止", "停止播放", "取消",
                                            "stop", "cancel", "quit"};
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

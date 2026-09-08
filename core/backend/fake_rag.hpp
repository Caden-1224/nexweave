// 确定性 Fake RAG（Retrieval-Augmented Generation，检索增强生成）及 L0-L3 路由。
// 本文件只依赖领域值对象，不创建线程、文件、socket 或模型句柄；所有夹具数据由对象
// 拥有并在析构时释放。实现用于 WSL/Mock 回归，不宣称真实知识库的准确率或分数概率。
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "../capability/backend.hpp"
namespace nexweave::backend {
enum class RagRouteLevel { kL0, kL1, kL2, kL3 };
// 路由结果同时保留检索证据和执行建议，便于审计而不把 score 当作概率。
// L0 的 control_action 非空；L1 的 direct_answer 使用首个命中文本；L2/L3 由上层
// 分别决定是否把 hits 注入 LLM。结果只拥有字符串副本，不借用检索器内部内存。
struct RagRouteDecision {
  RagRouteLevel level = RagRouteLevel::kL3;
  std::vector<capability::RetrievedChunk> hits;
  std::string reason;
  std::string control_action;
  std::string direct_answer;
};
// Fake 检索夹具。score 由夹具预先给定，必须为有限非负数；返回按 score 降序、id
// 字典序稳定排序。空 query 返回 kInvalidInput，top_k=0 返回成功空列表；无命中也是成功。
class FakeRag final : public capability::IRag {
 public:
  explicit FakeRag(std::vector<capability::RetrievedChunk> chunks);
  domain::Result<std::vector<capability::RetrievedChunk>> retrieve(
      const std::string& query, std::size_t top_k) override;
 private:
  std::vector<capability::RetrievedChunk> chunks_;
};
// 路由器同步执行控制识别、检索和级别选择。cancel 是线性化状态：cancel 后 route
// 一律返回 kCancelled，reset 才开启下一轮；无外部资源和等待。调用方保证串行访问，且
// retriever 的生命周期长于本对象。
class FakeRagRouter final {
 public:
  struct Config {
    double direct_answer_threshold = 0.90;
    double context_threshold = 0.50;
    std::size_t top_k = 3;
  };
  FakeRagRouter(capability::IRag& retriever);
  FakeRagRouter(capability::IRag& retriever, Config config);
  domain::Result<RagRouteDecision> route(const std::string& query);
  domain::OperationResult cancel() noexcept;
  domain::OperationResult reset() noexcept;
 private:
  capability::IRag& retriever_;
  Config config_;
  bool cancelled_ = false;
};
}  // namespace nexweave::backend

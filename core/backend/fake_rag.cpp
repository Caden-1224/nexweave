#include "fake_rag.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace nexweave::backend {
namespace {

std::string normalize(std::string value) {
  std::string output;
  output.reserve(value.size());
  bool whitespace = false;
  for (const unsigned char character : value) {
    if (std::isspace(character) != 0) {
      whitespace = true;
      continue;
    }
    if (whitespace && !output.empty()) {
      output.push_back(' ');
    }
    whitespace = false;
    output.push_back(static_cast<char>(std::tolower(character)));
  }
  return output;
}

bool finite_non_negative(double value) {
  return std::isfinite(value) && value >= 0.0;
}

// L0 匹配采用“规范化后完全相等”，而不是子串搜索。这样控制词表可以刻意包含
// “停止播放”，同时避免把“取消后旧数据怎么处理”这种知识问题误判成停止指令。
bool is_control(const std::string& query, const FakeRagRouter::Config& config,
                std::string& action) {
  const std::string normalized_query = normalize(query);
  for (const auto& keyword : config.l0_keywords) {
    if (normalized_query == normalize(keyword)) {
      action = "cancel";
      return true;
    }
  }
  return false;
}

// 按分数顺序装入完整命中，直到下一条会超出 UTF-8 字节预算为止。跳过放不下的条目而
// 不是截断文本，是因为截断可能破坏 UTF-8 的中文边界；返回的每一条都是完整知识项。
// 若一条都放不下，调用方会改走 L3，确保不会把空或越界上下文交给模型。
std::vector<capability::RetrievedChunk> apply_context_budget(
    const std::vector<capability::RetrievedChunk>& hits,
    std::size_t max_context_bytes) {
  std::vector<capability::RetrievedChunk> selected;
  selected.reserve(hits.size());
  std::size_t used_bytes = 0;
  for (const auto& hit : hits) {
    if (hit.text.empty() || hit.text.size() > max_context_bytes - used_bytes) {
      continue;
    }
    selected.push_back(hit);
    used_bytes += hit.text.size();
    if (used_bytes == max_context_bytes) {
      break;
    }
  }
  return selected;
}

}  // namespace

FakeRag::FakeRag(std::vector<capability::RetrievedChunk> chunks)
    : chunks_(std::move(chunks)) {
  for (const auto& chunk : chunks_) {
    if (chunk.id.empty() || chunk.text.empty() || !finite_non_negative(chunk.score)) {
      throw std::invalid_argument("FakeRag chunks must have id, text and finite score");
    }
  }
}

domain::Result<std::vector<capability::RetrievedChunk>> FakeRag::retrieve(
    const std::string& query, std::size_t top_k) {
  const auto normalized = normalize(query);
  if (normalized.empty()) {
    return domain::Result<std::vector<capability::RetrievedChunk>>::failure(
        domain::ErrorCode::kInvalidInput, "query must not be empty");
  }
  if (top_k == 0) {
    return domain::Result<std::vector<capability::RetrievedChunk>>::success({});
  }

  std::vector<capability::RetrievedChunk> matches;
  for (const auto& chunk : chunks_) {
    const bool text_match =
        normalize(chunk.text).find(normalized) != std::string::npos;
    const bool id_match =
        normalize(chunk.id).find(normalized) != std::string::npos;
    if (text_match || id_match) {
      matches.push_back(chunk);
    }
  }
  std::stable_sort(matches.begin(), matches.end(),
                   [](const auto& left, const auto& right) {
                     if (left.score != right.score) {
                       return left.score > right.score;
                     }
                     return left.id < right.id;
                   });
  if (matches.size() > top_k) {
    matches.resize(top_k);
  }
  return domain::Result<std::vector<capability::RetrievedChunk>>::success(
      std::move(matches));
}

FakeRagRouter::FakeRagRouter(capability::IRag& retriever)
    : FakeRagRouter(retriever, Config{}) {}

FakeRagRouter::FakeRagRouter(capability::IRag& retriever, Config config)
    : retriever_(retriever), config_(std::move(config)) {
  if (!finite_non_negative(config_.direct_answer_threshold) ||
      !finite_non_negative(config_.context_threshold) ||
      config_.direct_answer_threshold < config_.context_threshold ||
      config_.top_k == 0 || config_.max_context_bytes == 0) {
    throw std::invalid_argument("invalid FakeRagRouter config");
  }
  if (config_.l0_keywords.empty()) {
    throw std::invalid_argument("FakeRagRouter must have at least one L0 keyword");
  }
  for (const auto& keyword : config_.l0_keywords) {
    if (normalize(keyword).empty()) {
      throw std::invalid_argument("FakeRagRouter L0 keyword must not normalize to empty");
    }
  }
}

domain::Result<RagRouteDecision> FakeRagRouter::route(const std::string& query) {
  if (cancelled_) {
    return domain::Result<RagRouteDecision>::failure(domain::ErrorCode::kCancelled);
  }
  const auto normalized = normalize(query);
  if (normalized.empty()) {
    return domain::Result<RagRouteDecision>::failure(
        domain::ErrorCode::kInvalidInput, "query must not be empty");
  }

  RagRouteDecision decision;
  if (is_control(query, config_, decision.control_action)) {
    decision.level = RagRouteLevel::kL0;
    decision.reason = "control intent bypasses retrieval and LLM";
    return domain::Result<RagRouteDecision>::success(std::move(decision));
  }

  auto retrieved = retriever_.retrieve(query, config_.top_k);
  if (!retrieved.ok()) {
    return domain::Result<RagRouteDecision>::failure(
        retrieved.error.code, retrieved.error.message);
  }
  decision.hits = std::move(*retrieved.value);
  if (decision.hits.empty()) {
    decision.level = RagRouteLevel::kL3;
    decision.reason = "no retrieval hit; use ordinary conversation";
  } else if (decision.hits.front().score >= config_.direct_answer_threshold) {
    decision.level = RagRouteLevel::kL1;
    decision.direct_answer = decision.hits.front().text;
    decision.reason = "top hit meets direct-answer threshold";
  } else if (decision.hits.front().score >= config_.context_threshold) {
    std::vector<capability::RetrievedChunk> original_hits = std::move(decision.hits);
    decision.hits = apply_context_budget(original_hits, config_.max_context_bytes);
    if (decision.hits.empty()) {
      decision.level = RagRouteLevel::kL3;
      decision.reason = "hits exceed context byte budget; use ordinary conversation";
    } else {
      decision.level = RagRouteLevel::kL2;
      decision.reason = decision.hits.size() == original_hits.size()
                            ? "top hit supplies context within byte budget"
                            : "top hits supply context within byte budget";
    }
  } else {
    decision.level = RagRouteLevel::kL3;
    decision.reason = "hits are below context threshold";
  }
  return domain::Result<RagRouteDecision>::success(std::move(decision));
}

domain::OperationResult FakeRagRouter::cancel() noexcept {
  cancelled_ = true;
  return domain::OperationResult::success();
}

domain::OperationResult FakeRagRouter::reset() noexcept {
  cancelled_ = false;
  return domain::OperationResult::success();
}

}  // namespace nexweave::backend

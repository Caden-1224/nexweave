#include "fake_rag.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
namespace nexweave::backend {
namespace {
std::string normalize(std::string value) {
  std::string output;
  output.reserve(value.size());
  bool whitespace = false;
  for (const unsigned char character : value) {
    if (std::isspace(character) != 0) { whitespace = true; continue; }
    if (whitespace && !output.empty()) output.push_back(' ');
    whitespace = false;
    output.push_back(static_cast<char>(std::tolower(character)));
  }
  return output;
}
bool finite_non_negative(double value) { return std::isfinite(value) && value >= 0.0; }
bool is_control(const std::string& query, std::string& action) {
  const auto normalized = normalize(query);
  if (normalized == "停止" || normalized == "取消" || normalized == "stop" ||
      normalized == "cancel") { action = "cancel"; return true; }
  return false;
}
}  // namespace
FakeRag::FakeRag(std::vector<capability::RetrievedChunk> chunks) : chunks_(std::move(chunks)) {
  for (const auto& chunk : chunks_) {
    if (chunk.id.empty() || chunk.text.empty() || !finite_non_negative(chunk.score))
      throw std::invalid_argument("FakeRag chunks must have id, text and finite score");
  }
}
domain::Result<std::vector<capability::RetrievedChunk>> FakeRag::retrieve(
    const std::string& query, std::size_t top_k) {
  if (query.empty())
    return domain::Result<std::vector<capability::RetrievedChunk>>::failure(
        domain::ErrorCode::kInvalidInput, "query must not be empty");
  if (top_k == 0)
    return domain::Result<std::vector<capability::RetrievedChunk>>::success({});
  const auto normalized = normalize(query);
  std::vector<capability::RetrievedChunk> matches;
  for (const auto& chunk : chunks_) {
    if (normalize(chunk.text).find(normalized) != std::string::npos ||
        normalize(chunk.id).find(normalized) != std::string::npos) matches.push_back(chunk);
  }
  std::stable_sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
    if (left.score != right.score) return left.score > right.score;
    return left.id < right.id;
  });
  if (matches.size() > top_k) matches.resize(top_k);
  return domain::Result<std::vector<capability::RetrievedChunk>>::success(std::move(matches));
}
FakeRagRouter::FakeRagRouter(capability::IRag& retriever) : FakeRagRouter(retriever, Config{}) {}

FakeRagRouter::FakeRagRouter(capability::IRag& retriever, Config config)
    : retriever_(retriever), config_(config) {
  if (!finite_non_negative(config_.direct_answer_threshold) ||
      !finite_non_negative(config_.context_threshold) ||
      config_.direct_answer_threshold < config_.context_threshold || config_.top_k == 0)
    throw std::invalid_argument("invalid FakeRagRouter config");
}
domain::Result<RagRouteDecision> FakeRagRouter::route(const std::string& query) {
  if (cancelled_) return domain::Result<RagRouteDecision>::failure(domain::ErrorCode::kCancelled);
  if (query.empty())
    return domain::Result<RagRouteDecision>::failure(domain::ErrorCode::kInvalidInput,
                                                     "query must not be empty");
  RagRouteDecision decision;
  if (is_control(query, decision.control_action)) {
    decision.level = RagRouteLevel::kL0;
    decision.reason = "control intent bypasses retrieval and LLM";
    return domain::Result<RagRouteDecision>::success(std::move(decision));
  }
  auto retrieved = retriever_.retrieve(query, config_.top_k);
  if (!retrieved.ok())
    return domain::Result<RagRouteDecision>::failure(retrieved.error.code, retrieved.error.message);
  decision.hits = std::move(*retrieved.value);
  if (decision.hits.empty()) {
    decision.level = RagRouteLevel::kL3;
    decision.reason = "no retrieval hit; use ordinary conversation";
  } else if (decision.hits.front().score >= config_.direct_answer_threshold) {
    decision.level = RagRouteLevel::kL1;
    decision.direct_answer = decision.hits.front().text;
    decision.reason = "top hit meets direct-answer threshold";
  } else if (decision.hits.front().score >= config_.context_threshold) {
    decision.level = RagRouteLevel::kL2;
    decision.reason = "top hit supplies context for LLM";
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

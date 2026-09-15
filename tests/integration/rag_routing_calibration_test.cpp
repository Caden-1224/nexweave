#include "../test_support.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "bm25_rag.hpp"
#include "fake_rag.hpp"

using namespace nexweave;
using backend::Bm25Options;
using backend::Bm25Rag;
using backend::FakeRagRouter;
using backend::RagRouteLevel;

namespace {

constexpr double kDirectAnswerThreshold = 8.0;
constexpr double kContextThreshold = 4.0;
constexpr std::size_t kTopK = 3;
constexpr std::size_t kMaxContextBytes = 512;

struct QueryCase {
  std::string query;
  std::string expected_level;
  std::string note;
};

struct RouteObservation {
  std::string query;
  std::string expected_level;
  std::string actual_level;
  double top1_score = 0.0;
  std::string note;
};

std::string SourcePath(const std::string& relative) {
  return std::string(NEXWEAVE_SOURCE_DIR) + "/" + relative;
}

std::string LevelName(RagRouteLevel level) {
  switch (level) {
    case RagRouteLevel::kL0:
      return "l0";
    case RagRouteLevel::kL1:
      return "l1";
    case RagRouteLevel::kL2:
      return "l2";
    case RagRouteLevel::kL3:
      return "l3";
  }
  return "unknown";
}

std::string Fingerprint(const std::string& text) {
  constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  std::uint64_t hash = kOffsetBasis;
  for (const unsigned char byte : text) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= kPrime;
  }
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "fnv1a64:%016llx",
                static_cast<unsigned long long>(hash));
  return std::string(buffer);
}

std::string ReadFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream content;
  content << input.rdbuf();
  return content.str();
}

std::vector<QueryCase> LoadCases(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open query set: " + path);
  }
  std::vector<QueryCase> cases;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::size_t begin = line.find_first_not_of(" \t");
    if (begin == std::string::npos || line[begin] == '#') {
      continue;
    }
    nlohmann::json object;
    try {
      object = nlohmann::json::parse(line.substr(begin));
    } catch (const nlohmann::json::exception&) {
      throw std::runtime_error(path + ":" + std::to_string(line_number) +
                               " invalid JSON object");
    }
    QueryCase item;
    item.query = object.value("query", std::string());
    item.expected_level = object.value("expected_level", std::string());
    item.note = object.value("note", std::string());
    if (item.query.empty() || item.expected_level.empty()) {
      throw std::runtime_error(path + ":" + std::to_string(line_number) +
                               " missing query or expected_level");
    }
    cases.push_back(std::move(item));
  }
  if (cases.empty()) {
    throw std::runtime_error("query set contains no cases: " + path);
  }
  return cases;
}

std::string ConfigFingerprint(const FakeRagRouter::Config& config) {
  std::ostringstream text;
  text << "tokenization=" << backend::kSearchTokenizationVersion << '\n';
  text << "k1=1.2\nb=0.6\n";
  text << "direct=" << config.direct_answer_threshold << '\n';
  text << "context=" << config.context_threshold << '\n';
  text << "top_k=" << config.top_k << '\n';
  text << "max_context_bytes=" << config.max_context_bytes << '\n';
  for (const auto& keyword : config.l0_keywords) {
    text << "l0=" << keyword << '\n';
  }
  return Fingerprint(text.str());
}

std::vector<RouteObservation> RunSet(
    const std::string& path,
    FakeRagRouter& router,
    std::size_t& failure_count,
    std::vector<std::string>& failures) {
  const std::vector<QueryCase> cases = LoadCases(path);
  std::vector<RouteObservation> observations;
  for (const auto& item : cases) {
    const auto decision = router.route(item.query);
    if (!decision.ok()) {
      ++failure_count;
      failures.push_back(item.query + " -> error " +
                         std::to_string(static_cast<int>(decision.error.code)));
      continue;
    }
    RouteObservation observation;
    observation.query = item.query;
    observation.expected_level = item.expected_level;
    observation.actual_level = LevelName(decision.value->level);
    observation.top1_score =
        decision.value->hits.empty() ? 0.0 : decision.value->hits.front().score;
    observation.note = item.note;
    observations.push_back(observation);

    if (observation.actual_level != observation.expected_level) {
      ++failure_count;
      failures.push_back(item.query + ": expected " + observation.expected_level +
                         " actual " + observation.actual_level + " top1=" +
                         std::to_string(observation.top1_score));
    }

    // 保护不变量：L0 不携带任何检索命中；L1 必须有可直接回答的文本；L2 的每一条
    // 上下文都完整且总 UTF-8 字节数不超过显式预算，避免把空或越界上下文送给模型。
    if (decision.value->level == RagRouteLevel::kL0) {
      if (!decision.value->hits.empty() || decision.value->control_action.empty()) {
        ++failure_count;
        failures.push_back(item.query + ": L0 evidence is inconsistent");
      }
    }
    if (decision.value->level == RagRouteLevel::kL1 && decision.value->direct_answer.empty()) {
      ++failure_count;
      failures.push_back(item.query + ": L1 direct answer is empty");
    }
    if (decision.value->level == RagRouteLevel::kL2) {
      std::size_t context_bytes = 0;
      for (const auto& hit : decision.value->hits) {
        context_bytes += hit.text.size();
      }
      if (decision.value->hits.empty() || context_bytes == 0 ||
          context_bytes > kMaxContextBytes) {
        ++failure_count;
        failures.push_back(item.query + ": L2 context budget violated");
      }
    }
    if (item.query == "你好" && !decision.value->hits.empty()) {
      ++failure_count;
      failures.push_back("no-hit query unexpectedly produced retrieval hits");
    }
  }
  return observations;
}

void PrintObservations(const char* set_name,
                       const std::vector<RouteObservation>& observations) {
  std::cout << "== " << set_name << " ==\n";
  for (const auto& item : observations) {
    std::cout << item.query << " | expected=" << item.expected_level
              << " actual=" << item.actual_level << " top1=" << item.top1_score
              << " | " << item.note << '\n';
  }
}

}  // namespace

int main() {
  const std::string knowledge_path = SourcePath("data/knowledge/nexweave_knowledge.jsonl");
  const std::string calibration_path =
      SourcePath("data/knowledge/routing_calibration.jsonl");
  const std::string acceptance_path =
      SourcePath("data/knowledge/routing_acceptance.jsonl");

  auto knowledge = backend::load_knowledge_jsonl(knowledge_path);
  CHECK(knowledge.ok());
  Bm25Rag retriever(*knowledge.value, Bm25Options{1.2, 0.6, 4096});

  FakeRagRouter::Config config;
  config.direct_answer_threshold = kDirectAnswerThreshold;
  config.context_threshold = kContextThreshold;
  config.top_k = kTopK;
  config.max_context_bytes = kMaxContextBytes;
  FakeRagRouter router(retriever, config);

  const std::string calibration_text = ReadFile(calibration_path);
  const std::string acceptance_text = ReadFile(acceptance_path);
  std::cout << "knowledge_index=" << retriever.index_version() << '\n';
  std::cout << "calibration_set_hash=" << Fingerprint(calibration_text) << '\n';
  std::cout << "acceptance_set_hash=" << Fingerprint(acceptance_text) << '\n';
  std::cout << "router_config_hash=" << ConfigFingerprint(config) << '\n';
  std::cout << "direct_threshold=" << config.direct_answer_threshold
            << " context_threshold=" << config.context_threshold
            << " top_k=" << config.top_k
            << " max_context_bytes=" << config.max_context_bytes << '\n';

  std::size_t failure_count = 0;
  std::vector<std::string> failures;
  const auto calibration = RunSet(calibration_path, router, failure_count, failures);
  const auto acceptance = RunSet(acceptance_path, router, failure_count, failures);
  PrintObservations("calibration", calibration);
  PrintObservations("acceptance", acceptance);

  std::cout << "summary: calibration=" << calibration.size()
            << " acceptance=" << acceptance.size()
            << " failures=" << failure_count << '\n';
  if (failure_count != 0) {
    std::string detail = "routing calibration/acceptance failure(s):";
    for (const auto& failure : failures) {
      detail += "\n  " + failure;
    }
    CHECK_MESSAGE(false, detail);
  }
}

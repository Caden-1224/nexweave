#include "../test_support.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "bm25_rag.hpp"

using namespace nexweave;
using backend::Bm25Options;
using backend::Bm25Rag;
using backend::KnowledgeBase;
using backend::KnowledgeItem;
using backend::KnowledgeLoadLimits;

namespace {

std::string TempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() /
          ("nexweave-bm25-" + name + "-" + std::to_string(::getpid()) + ".jsonl"))
      .string();
}

void WriteText(const std::string& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary);
  output << text;
}

KnowledgeBase MakeKnowledge(std::vector<KnowledgeItem> items) {
  KnowledgeBase knowledge;
  knowledge.items = std::move(items);
  return knowledge;
}

bool ThrowsInvalidArgument(const std::function<void()>& call) {
  try {
    call();
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

// 保护不变量：规范化与分词是版本化、确定性的；中文逐字、ASCII 连续段整体、全角折叠、
// 标点只作边界。该规则变化会同时改变 BM25 分数和所有校准阈值。
void TestNormalizationAndTokenization() {
  const auto tokens = backend::tokenize_search_text("你好 world 42");
  CHECK(tokens.size() == 4);
  CHECK(tokens[0] == "你");
  CHECK(tokens[1] == "好");
  CHECK(tokens[2] == "world");
  CHECK(tokens[3] == "42");

  CHECK(backend::normalize_search_text("ＡＢＣ") == "abc");
  CHECK(backend::normalize_search_text("a  b\n\tc") == "a b c");
  const auto punctuation = backend::tokenize_search_text("cat,dog!");
  CHECK(punctuation.size() == 2);
  CHECK(punctuation[0] == "cat");
  CHECK(punctuation[1] == "dog");
}

// 保护不变量：文件缺失、非法 JSON、缺字段、空字段、重复 id、空知识库都有确定错误，
// 并且错误不产生半加载的知识库。查询失败同样不得消费旧数据。
void TestLoaderValidation() {
  const std::string valid_path = TempPath("valid");
  WriteText(valid_path,
            "# comment\n\n"
            "{\"id\":\"a\",\"text\":\"alpha beta\"}\n"
            "{\"id\":\"b\",\"text\":\"beta gamma\"}\n");
  auto loaded = backend::load_knowledge_jsonl(valid_path);
  CHECK(loaded.ok());
  CHECK(loaded.value->items.size() == 2);
  CHECK(loaded.value->format_version == backend::kKnowledgeFormatVersion);
  CHECK(loaded.value->content_fingerprint.find("fnv1a64:") == 0);
  CHECK(loaded.value->index_version().find(backend::kKnowledgeFormatVersion) == 0);
  std::remove(valid_path.c_str());

  const auto missing = backend::load_knowledge_jsonl(TempPath("missing"));
  CHECK(missing.error.code == domain::ErrorCode::kDeviceFailure);

  const std::string bad_json_path = TempPath("bad-json");
  WriteText(bad_json_path, "{\"id\":\"a\",\"text\":}\n");
  const auto bad_json = backend::load_knowledge_jsonl(bad_json_path);
  CHECK(bad_json.error.code == domain::ErrorCode::kInvalidInput);
  CHECK(bad_json.error.message.find(":1") != std::string::npos);
  std::remove(bad_json_path.c_str());

  const std::string missing_field_path = TempPath("missing-field");
  WriteText(missing_field_path, "{\"id\":\"a\"}\n");
  const auto missing_field = backend::load_knowledge_jsonl(missing_field_path);
  CHECK(missing_field.error.code == domain::ErrorCode::kMissingField);
  std::remove(missing_field_path.c_str());

  const std::string blank_path = TempPath("blank");
  WriteText(blank_path, "{\"id\":\"a\",\"text\":\"   \"}\n");
  const auto blank = backend::load_knowledge_jsonl(blank_path);
  CHECK(blank.error.code == domain::ErrorCode::kInvalidInput);
  std::remove(blank_path.c_str());

  const std::string duplicate_path = TempPath("duplicate");
  WriteText(duplicate_path,
            "{\"id\":\"a\",\"text\":\"alpha\"}\n"
            "{\"id\":\"a\",\"text\":\"beta\"}\n");
  const auto duplicate = backend::load_knowledge_jsonl(duplicate_path);
  CHECK(duplicate.error.code == domain::ErrorCode::kInvalidInput);
  std::remove(duplicate_path.c_str());

  const std::string unknown_path = TempPath("unknown");
  WriteText(unknown_path, "{\"id\":\"a\",\"text\":\"alpha\",\"extra\":1}\n");
  const auto unknown = backend::load_knowledge_jsonl(unknown_path);
  CHECK(unknown.error.code == domain::ErrorCode::kInvalidInput);
  std::remove(unknown_path.c_str());

  const std::string empty_path = TempPath("empty");
  WriteText(empty_path, "# only a comment\n");
  const auto empty = backend::load_knowledge_jsonl(empty_path);
  CHECK(empty.error.code == domain::ErrorCode::kInvalidInput);
  std::remove(empty_path.c_str());

  KnowledgeLoadLimits limits;
  limits.max_items = 1U;
  const std::string limited_path = TempPath("limited");
  WriteText(limited_path,
            "{\"id\":\"a\",\"text\":\"alpha\"}\n"
            "{\"id\":\"b\",\"text\":\"beta\"}\n");
  const auto limited = backend::load_knowledge_jsonl(limited_path, limits);
  CHECK(limited.error.code == domain::ErrorCode::kInvalidInput);
  std::remove(limited_path.c_str());
}

// 保护不变量：BM25 分值可由公式复算；排序全序（分数降序、同分 id 升序）；top_k 截断
// 发生在完整排序之后；分数不会被包装成概率。
void TestBm25HandFormulaAndTieOrder() {
  Bm25Rag rag(MakeKnowledge({{"a", "alpha beta"},
                             {"b", "alpha gamma"},
                             {"c", "beta gamma"}}),
              Bm25Options{1.2, 0.6, 4096});

  const double expected_idf = std::log(1.6);
  const auto alpha = rag.retrieve("alpha", 3);
  CHECK(alpha.ok());
  CHECK(alpha.value->size() == 2);
  CHECK((*alpha.value)[0].id == "a");
  CHECK((*alpha.value)[1].id == "b");
  CHECK(std::fabs((*alpha.value)[0].score - expected_idf) < 1e-12);
  CHECK(std::fabs((*alpha.value)[1].score - expected_idf) < 1e-12);

  const auto pair = rag.retrieve("alpha beta", 3);
  CHECK(pair.ok());
  CHECK(pair.value->size() == 3);
  CHECK((*pair.value)[0].id == "a");
  CHECK(std::fabs((*pair.value)[0].score - 2.0 * expected_idf) < 1e-12);

  const auto limited = rag.retrieve("beta", 1);
  CHECK(limited.ok());
  CHECK(limited.value->size() == 1);
  CHECK((*limited.value)[0].id == "a");

  const auto zero = rag.retrieve("alpha", 0);
  CHECK(zero.ok());
  CHECK(zero.value->empty());
  CHECK(rag.retrieve("", 1).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(rag.retrieve("   ", 1).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(rag.retrieve(",", 1).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(rag.retrieve("ＡＬＰＨＡ", 1).ok());
  CHECK((*rag.retrieve("ＡＬＰＨＡ", 1).value)[0].id == "a");
}

// 保护不变量：id 相同时同分顺序不能依赖输入行序或哈希表迭代顺序。
void TestEqualScoresAreOrderedById() {
  Bm25Rag rag(MakeKnowledge({{"z", "same text"},
                             {"a", "same text"},
                             {"m", "same text"}}),
              Bm25Options{1.2, 0.6, 4096});
  const auto hits = rag.retrieve("same", 3);
  CHECK(hits.ok());
  CHECK(hits.value->size() == 3);
  CHECK((*hits.value)[0].id == "a");
  CHECK((*hits.value)[1].id == "m");
  CHECK((*hits.value)[2].id == "z");
}

// 保护不变量：构造期验证所有会改变索引语义的输入；非法选项、空库、重复 id、空字段
// 或无法分词文本都不得进入“可检索”状态。
void TestConstructionBoundaries() {
  CHECK(ThrowsInvalidArgument([] {
    Bm25Rag rag(KnowledgeBase{}, Bm25Options{});
  }));
  CHECK(ThrowsInvalidArgument([] {
    Bm25Rag rag(MakeKnowledge({{"a", "alpha"}, {"a", "beta"}}), Bm25Options{});
  }));
  CHECK(ThrowsInvalidArgument([] {
    Bm25Rag rag(MakeKnowledge({{"", "alpha"}}), Bm25Options{});
  }));
  CHECK(ThrowsInvalidArgument([] {
    Bm25Rag rag(MakeKnowledge({{"a", "   "}}), Bm25Options{});
  }));
  CHECK(ThrowsInvalidArgument([] {
    Bm25Rag rag(MakeKnowledge({{"a", "alpha"}}), Bm25Options{0.0, 0.6, 4096});
  }));
  CHECK(ThrowsInvalidArgument([] {
    Bm25Rag rag(MakeKnowledge({{"a", "alpha"}}), Bm25Options{1.2, 1.5, 4096});
  }));
  CHECK(ThrowsInvalidArgument([] {
    Bm25Rag rag(MakeKnowledge({{"a", "alpha"}}), Bm25Options{1.2, 0.6, 0});
  }));

  Bm25Rag rag(MakeKnowledge({{"a", "alpha"}}), Bm25Options{1.2, 0.6, 4});
  CHECK(rag.retrieve("alpha", 1).error.code == domain::ErrorCode::kInvalidInput);
  CHECK(rag.document_count() == 1);
  CHECK(std::string(rag.tokenization_version()) == backend::kSearchTokenizationVersion);
}

// 保护不变量：重载采用“先构造新对象、成功后再切换”的外部策略；一次失败加载不能把
// 旧对象改成半更新状态，旧对象仍能返回与失败前相同的命中。
void TestFailedReloadDoesNotMutateExistingIndex() {
  Bm25Rag rag(MakeKnowledge({{"a", "alpha"}}), Bm25Options{});
  const auto before = rag.retrieve("alpha", 1);
  CHECK(before.ok());
  CHECK(before.value->size() == 1);

  const auto failed = backend::load_knowledge_jsonl(TempPath("reload-missing"));
  CHECK(failed.error.code == domain::ErrorCode::kDeviceFailure);

  const auto after = rag.retrieve("alpha", 1);
  CHECK(after.ok());
  CHECK(after.value->size() == 1);
  CHECK((*after.value)[0].id == "a");
  CHECK((*after.value)[0].score == (*before.value)[0].score);
}

}  // namespace

int main() {
  TestNormalizationAndTokenization();
  TestLoaderValidation();
  TestBm25HandFormulaAndTieOrder();
  TestEqualScoresAreOrderedById();
  TestConstructionBoundaries();
  TestFailedReloadDoesNotMutateExistingIndex();
}

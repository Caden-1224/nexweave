#include "bm25_rag.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

namespace nexweave::backend {
namespace {

constexpr char32_t kInvalidCodePoint = 0xFFFD;

// 把一个 UTF-8 码点解码成 Unicode 标量值。函数始终前进至少一个字节，丢失上下文时把
// 当前非法字节当作一个“非 token 边界”处理，因此后续校验和分词不会因坏输入而失败或
// 越界。返回位置总是满足 position < input.size() 时前进。
std::size_t decode_next(const std::string& input, std::size_t position, char32_t& code_point) {
  const auto lead = static_cast<unsigned char>(input[position]);
  if (lead < 0x80U) {
    code_point = static_cast<char32_t>(lead);
    return position + 1U;
  }

  std::size_t continuation_count = 0;
  char32_t value = 0;
  if ((lead & 0xE0U) == 0xC0U) {
    continuation_count = 1U;
    value = static_cast<char32_t>(lead & 0x1FU);
  } else if ((lead & 0xF0U) == 0xE0U) {
    continuation_count = 2U;
    value = static_cast<char32_t>(lead & 0x0FU);
  } else if ((lead & 0xF8U) == 0xF0U) {
    continuation_count = 3U;
    value = static_cast<char32_t>(lead & 0x07U);
  } else {
    code_point = kInvalidCodePoint;
    return position + 1U;
  }

  if (position + continuation_count >= input.size()) {
    code_point = kInvalidCodePoint;
    return position + 1U;
  }
  for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
    const auto byte = static_cast<unsigned char>(input[position + offset]);
    if ((byte & 0xC0U) != 0x80U) {
      code_point = kInvalidCodePoint;
      return position + 1U;
    }
    value = static_cast<char32_t>((value << 6U) | (byte & 0x3FU));
  }

  const bool overlong = (continuation_count == 1U && value < 0x80U) ||
                        (continuation_count == 2U && value < 0x800U) ||
                        (continuation_count == 3U && value < 0x10000U);
  const bool surrogate = value >= 0xD800U && value <= 0xDFFFU;
  const bool out_of_range = value > 0x10FFFFU;
  if (overlong || surrogate || out_of_range) {
    code_point = kInvalidCodePoint;
    return position + 1U;
  }

  code_point = value;
  return position + continuation_count + 1U;
}

// 全角 ASCII 与全角空格折半角。该规则只覆盖检索需要的字符范围；不改动 CJK 和其他
// Unicode 字符，避免在没有完整 NFKC 实现时制造不可解释的差异。
char32_t fold_full_width(char32_t code_point) {
  if (code_point >= 0xFF01U && code_point <= 0xFF5EU) {
    return code_point - 0xFEE0U;
  }
  if (code_point == 0x3000U) {
    return static_cast<char32_t>(' ');
  }
  return code_point;
}

bool is_ascii_alnum(char32_t code_point) {
  if (code_point > 0x7FU) {
    return false;
  }
  return std::isalnum(static_cast<unsigned char>(code_point)) != 0;
}

bool is_cjk_basic(char32_t code_point) {
  return code_point >= 0x4E00U && code_point <= 0x9FFFU;
}

bool is_ascii_space(char32_t code_point) {
  return code_point == static_cast<char32_t>(' ') ||
         code_point == static_cast<char32_t>('\t') ||
         code_point == static_cast<char32_t>('\n') ||
         code_point == static_cast<char32_t>('\r');
}

bool is_search_token_character(char32_t code_point) {
  return is_ascii_alnum(code_point) || is_cjk_basic(code_point);
}

void append_utf8(std::string& output, char32_t code_point) {
  if (code_point < 0x80U) {
    output.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800U) {
    output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
    output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else if (code_point < 0x10000U) {
    output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else {
    output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  }
}

bool has_visible_text(const std::string& text) {
  for (const unsigned char byte : text) {
    if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
      return true;
    }
  }
  return false;
}

std::string trim_copy(const std::string& text) {
  const std::size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const std::size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1U);
}

std::string fingerprint_items(const std::vector<KnowledgeItem>& items) {
  // 对规范化条目内容做 FNV-1a 64，而不是对原文件逐字节哈希。这样字段顺序、空白和
  // 行尾差异不会改变同一个逻辑知识库的指纹，索引版本与内容语义保持一致。
  constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  std::uint64_t hash = kOffsetBasis;

  const std::string canonical_prefix = std::string(kKnowledgeFormatVersion) + "\n";
  for (const unsigned char byte : canonical_prefix) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= kPrime;
  }
  for (const auto& item : items) {
    const std::string record = item.id + '\x1F' + item.text + '\n';
    for (const unsigned char byte : record) {
      hash ^= static_cast<std::uint64_t>(byte);
      hash *= kPrime;
    }
  }

  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "fnv1a64:%016llx",
                static_cast<unsigned long long>(hash));
  return std::string(buffer);
}

template <typename T>
domain::Result<T> load_failure(domain::ErrorCode code, const std::string& message) {
  return domain::Result<T>::failure(code, message);
}

}  // namespace

std::string KnowledgeBase::index_version() const {
  if (content_fingerprint.empty()) {
    return format_version;
  }
  return format_version + ":" + content_fingerprint;
}

std::string normalize_search_text(const std::string& text) {
  std::string output;
  output.reserve(text.size());
  bool pending_space = false;
  std::size_t position = 0;
  while (position < text.size()) {
    char32_t code_point = 0;
    position = decode_next(text, position, code_point);
    if (code_point == kInvalidCodePoint) {
      pending_space = !output.empty();
      continue;
    }
    code_point = fold_full_width(code_point);
    if (is_ascii_space(code_point)) {
      if (!output.empty()) {
        pending_space = true;
      }
      continue;
    }
    if (!is_search_token_character(code_point)) {
      pending_space = !output.empty();
      continue;
    }
    if (pending_space && !output.empty()) {
      output.push_back(' ');
    }
    pending_space = false;
    if (code_point <= 0x7FU) {
      output.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(code_point))));
    } else {
      append_utf8(output, code_point);
    }
  }
  return output;
}

std::vector<std::string> tokenize_search_text(const std::string& text) {
  std::vector<std::string> tokens;
  std::string ascii_run;
  std::size_t position = 0;
  while (position < text.size()) {
    char32_t code_point = 0;
    position = decode_next(text, position, code_point);
    code_point = fold_full_width(code_point);
    if (is_ascii_alnum(code_point)) {
      ascii_run.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(code_point))));
      continue;
    }
    if (!ascii_run.empty()) {
      tokens.push_back(ascii_run);
      ascii_run.clear();
    }
    if (is_cjk_basic(code_point)) {
      std::string token;
      append_utf8(token, code_point);
      tokens.push_back(std::move(token));
    }
  }
  if (!ascii_run.empty()) {
    tokens.push_back(std::move(ascii_run));
  }
  return tokens;
}

domain::Result<KnowledgeBase> load_knowledge_jsonl(
    const std::string& path,
    const KnowledgeLoadLimits& limits) {
  if (path.empty()) {
    return load_failure<KnowledgeBase>(domain::ErrorCode::kInvalidInput,
                                       "knowledge path must not be empty");
  }
  if (limits.max_file_bytes == 0U || limits.max_items == 0U ||
      limits.max_id_bytes == 0U || limits.max_text_bytes == 0U) {
    return load_failure<KnowledgeBase>(domain::ErrorCode::kInvalidInput,
                                       "knowledge load limits must be non-zero");
  }

  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return load_failure<KnowledgeBase>(domain::ErrorCode::kDeviceFailure,
                                       "cannot open knowledge file: " + path);
  }
  const std::streamoff file_size = input.tellg();
  if (file_size < 0) {
    return load_failure<KnowledgeBase>(domain::ErrorCode::kDeviceFailure,
                                       "cannot determine knowledge file size: " + path);
  }
  if (static_cast<std::uintmax_t>(file_size) > limits.max_file_bytes) {
    return load_failure<KnowledgeBase>(
        domain::ErrorCode::kInvalidInput,
        "knowledge file exceeds max_file_bytes: " + path);
  }

  std::string content(static_cast<std::size_t>(file_size), '\0');
  input.seekg(0, std::ios::beg);
  if (!input && file_size != 0) {
    return load_failure<KnowledgeBase>(domain::ErrorCode::kDeviceFailure,
                                       "cannot seek knowledge file: " + path);
  }
  if (file_size != 0) {
    input.read(content.data(), file_size);
    if (input.gcount() != file_size) {
      return load_failure<KnowledgeBase>(domain::ErrorCode::kDeviceFailure,
                                         "cannot read knowledge file: " + path);
    }
  }

  KnowledgeBase knowledge;
  knowledge.format_version = kKnowledgeFormatVersion;
  std::unordered_set<std::string> seen_ids;
  std::istringstream lines(content);
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(lines, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::string trimmed = trim_copy(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    nlohmann::json object;
    try {
      object = nlohmann::json::parse(trimmed);
    } catch (const nlohmann::json::exception&) {
      return load_failure<KnowledgeBase>(
          domain::ErrorCode::kInvalidInput,
          path + ":" + std::to_string(line_number) + " invalid JSON object");
    }
    if (!object.is_object()) {
      return load_failure<KnowledgeBase>(
          domain::ErrorCode::kInvalidInput,
          path + ":" + std::to_string(line_number) + " knowledge line must be a JSON object");
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
      if (it.key() != "id" && it.key() != "text") {
        return load_failure<KnowledgeBase>(
            domain::ErrorCode::kInvalidInput,
            path + ":" + std::to_string(line_number) + " unknown field: " + it.key());
      }
    }

    const auto id_it = object.find("id");
    if (id_it == object.end()) {
      return load_failure<KnowledgeBase>(
          domain::ErrorCode::kMissingField,
          path + ":" + std::to_string(line_number) + " missing required field id");
    }
    const auto text_it = object.find("text");
    if (text_it == object.end()) {
      return load_failure<KnowledgeBase>(
          domain::ErrorCode::kMissingField,
          path + ":" + std::to_string(line_number) + " missing required field text");
    }
    if (!id_it->is_string() || !text_it->is_string()) {
      return load_failure<KnowledgeBase>(
          domain::ErrorCode::kInvalidInput,
          path + ":" + std::to_string(line_number) + " id and text must be strings");
    }

    KnowledgeItem item{id_it->get<std::string>(), text_it->get<std::string>()};
    if (item.id.empty() || !has_visible_text(item.id)) {
      return load_failure<KnowledgeBase>(
          domain::ErrorCode::kInvalidInput,
          path + ":" + std::to_string(line_number) + " id must not be blank");
    }
    if (item.text.empty() || !has_visible_text(item.text)) {
      return load_failure<KnowledgeBase>(
          domain::ErrorCode::kInvalidInput,
          path + ":" + std::to_string(line_number) + " text must not be blank");
    }
    if (item.id.size() > limits.max_id_bytes) {
      return load_failure<KnowledgeBase>(
          domain::ErrorCode::kInvalidInput,
          path + ":" + std::to_string(line_number) + " item exceeds max_id_bytes");
    }
    if (item.text.size() > limits.max_text_bytes) {
      return load_failure<KnowledgeBase>(
          domain::ErrorCode::kInvalidInput,
          path + ":" + std::to_string(line_number) + " item exceeds max_text_bytes");
    }
    if (!seen_ids.insert(item.id).second) {
      return load_failure<KnowledgeBase>(
          domain::ErrorCode::kInvalidInput,
          path + ":" + std::to_string(line_number) + " duplicate id: " + item.id);
    }
    if (knowledge.items.size() >= limits.max_items) {
      return load_failure<KnowledgeBase>(
          domain::ErrorCode::kInvalidInput,
          path + ":" + std::to_string(line_number) + " exceeds max_items");
    }
    knowledge.items.push_back(std::move(item));
  }

  if (knowledge.items.empty()) {
    return load_failure<KnowledgeBase>(
        domain::ErrorCode::kInvalidInput,
        "knowledge file contains no valid items: " + path);
  }
  knowledge.content_fingerprint = fingerprint_items(knowledge.items);
  return domain::Result<KnowledgeBase>::success(std::move(knowledge));
}

Bm25Rag::Bm25Rag(KnowledgeBase knowledge, Bm25Options options)
    : knowledge_(std::move(knowledge)), options_(options) {
  if (!std::isfinite(options_.k1) || options_.k1 <= 0.0) {
    throw std::invalid_argument("BM25 k1 must be finite and positive");
  }
  if (!std::isfinite(options_.b) || options_.b < 0.0 || options_.b > 1.0) {
    throw std::invalid_argument("BM25 b must be finite and within [0, 1]");
  }
  if (options_.max_query_bytes == 0U) {
    throw std::invalid_argument("BM25 max_query_bytes must be positive");
  }
  if (knowledge_.format_version != kKnowledgeFormatVersion) {
    throw std::invalid_argument("unsupported knowledge format version");
  }
  if (knowledge_.items.empty()) {
    throw std::invalid_argument("BM25 knowledge base must not be empty");
  }

  std::unordered_set<std::string> seen_ids;
  std::size_t total_tokens = 0;
  for (const auto& item : knowledge_.items) {
    if (item.id.empty() || !has_visible_text(item.id) || item.text.empty() ||
        !has_visible_text(item.text)) {
      throw std::invalid_argument("BM25 knowledge items must have non-blank id and text");
    }
    if (!seen_ids.insert(item.id).second) {
      throw std::invalid_argument("BM25 knowledge item ids must be unique");
    }

    std::vector<std::string> tokens = tokenize_search_text(item.text);
    if (tokens.empty()) {
      throw std::invalid_argument("BM25 knowledge item text must contain searchable tokens");
    }
    std::unordered_map<std::string, std::size_t> term_frequencies;
    for (const auto& token : tokens) {
      ++term_frequencies[token];
    }
    for (const auto& entry : term_frequencies) {
      ++document_frequency_[entry.first];
    }
    total_tokens += tokens.size();
    documents_.push_back(std::move(tokens));
    document_term_frequencies_.push_back(std::move(term_frequencies));
  }

  average_document_tokens_ =
      static_cast<double>(total_tokens) / static_cast<double>(documents_.size());
}

domain::Result<std::vector<capability::RetrievedChunk>> Bm25Rag::retrieve(
    const std::string& query,
    std::size_t top_k) {
  if (query.size() > options_.max_query_bytes) {
    return domain::Result<std::vector<capability::RetrievedChunk>>::failure(
        domain::ErrorCode::kInvalidInput, "query exceeds max_query_bytes");
  }

  const std::vector<std::string> raw_query_tokens = tokenize_search_text(query);
  if (raw_query_tokens.empty()) {
    return domain::Result<std::vector<capability::RetrievedChunk>>::failure(
        domain::ErrorCode::kInvalidInput, "query must contain searchable tokens");
  }
  if (top_k == 0U) {
    return domain::Result<std::vector<capability::RetrievedChunk>>::success({});
  }

  // 同一查询 token 只贡献一次 BM25 增量；重复 token 若重复计分，会让用户改一个词
  // 的重复次数就改变路由阈值，无法解释。排序时保持全序：分数降序、同分 id 升序。
  std::vector<std::string> query_tokens;
  query_tokens.reserve(raw_query_tokens.size());
  std::unordered_set<std::string> seen_tokens;
  for (const auto& token : raw_query_tokens) {
    if (seen_tokens.insert(token).second) {
      query_tokens.push_back(token);
    }
  }

  std::vector<capability::RetrievedChunk> hits;
  hits.reserve(documents_.size());
  for (std::size_t index = 0; index < documents_.size(); ++index) {
    const double score = score_document(query_tokens, index);
    if (score > 0.0) {
      hits.push_back(capability::RetrievedChunk{
          knowledge_.items[index].id,
          knowledge_.items[index].text,
          score,
      });
    }
  }

  std::sort(hits.begin(), hits.end(), [this](const auto& left, const auto& right) {
    if (left.score != right.score) {
      return left.score > right.score;
    }
    return left.id < right.id;
  });
  if (hits.size() > top_k) {
    hits.resize(top_k);
  }
  return domain::Result<std::vector<capability::RetrievedChunk>>::success(std::move(hits));
}

double Bm25Rag::score_document(const std::vector<std::string>& query_tokens,
                               std::size_t document_index) const {
  const std::size_t document_length = documents_[document_index].size();
  const double document_length_scale =
      average_document_tokens_ > 0.0
          ? static_cast<double>(document_length) / average_document_tokens_
          : 0.0;
  const double normalization =
      options_.k1 * (1.0 - options_.b + options_.b * document_length_scale);
  const double document_count = static_cast<double>(documents_.size());

  double score = 0.0;
  for (const auto& token : query_tokens) {
    const auto frequency_it = document_frequency_.find(token);
    if (frequency_it == document_frequency_.end()) {
      continue;
    }
    const auto term_it = document_term_frequencies_[document_index].find(token);
    if (term_it == document_term_frequencies_[document_index].end()) {
      continue;
    }

    const double document_frequency = static_cast<double>(frequency_it->second);
    const double term_frequency = static_cast<double>(term_it->second);
    const double inverse_document_frequency =
        std::log(1.0 + (document_count - document_frequency + 0.5) /
                           (document_frequency + 0.5));
    score += inverse_document_frequency *
             (term_frequency * (options_.k1 + 1.0)) /
             (term_frequency + normalization);
  }
  return score;
}

}  // namespace nexweave::backend

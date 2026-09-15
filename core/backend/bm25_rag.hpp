// 离线本地知识库与 BM25（Best Matching 25）词项检索。
//
// 职责与适用范围
// --------------
// 本文件把固定版本的 JSONL 知识库加载为不可变条目集合，并提供一个实现
// capability::IRag 的确定性检索后端。它只做词项统计与排序，不调用模型、网络、文件系统
// 写入或设备接口；加载之后对象不再读盘，因此检索可以在没有外部资源的路径上重复执行。
//
// 版本与兼容
// ----------
// JSONL 每行一个知识项，字段 id 与 text 都是必填的 UTF-8 字符串；v1 不接受额外字段，
// 避免字段拼写错误被静默忽略。知识库格式版本由 kKnowledgeFormatVersion 标识，分词与
// 规范化版本由 kSearchTokenizationVersion 标识。改变字段含义、分词规则或 BM25 公式时
// 必须提升相应版本，而不是复用旧指纹。
//
// 分数语义
// --------
// 返回的 score 是当前索引上的 BM25 排序量，不是概率，不能跨不同知识库、options 或
// 分词版本直接比较。调用方只能依赖“同一配置下排序稳定”和“阈值由同一数据集校准”这两点。
//
// 所有权、并发与重载
// ------------------
// KnowledgeBase 与 Bm25Rag 都按值拥有自己的派生数据，不持有文件句柄、线程或设备。
// retrieve 不修改对象，因此可以跨线程并发调用；调用方不得并发修改或销毁对象。本库不提供
// 原地 reload。重载必须在对象外重新加载并构造一个新的 Bm25Rag，失败时旧对象保持完整，
// 成功后再由拥有者在一个明确边界切换指针，从而避免半更新索引。
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "../capability/backend.hpp"

namespace nexweave::backend {

// 知识库 JSONL 的字段与校验规则版本。版本变化表示旧文件可能不再可加载，必须显式迁移。
inline constexpr char kKnowledgeFormatVersion[] = "nexweave-knowledge-jsonl-v1";

// 文本规范化与分词规则版本。版本变化会改变同一知识库上的 BM25 分数与阈值校准结果。
inline constexpr char kSearchTokenizationVersion[] = "nexweave-search-tokenization-v1";

// 一条通过校验的知识项。id 在同一个 KnowledgeBase 内唯一，text 是可供检索和注入
// 上下文的原始文本；两者都由对象按值拥有。
struct KnowledgeItem {
  std::string id;
  std::string text;
};

// 知识库加载限制。所有限制必须非零；加载器不会把超限文件静默截断，而是在建立任何索引
// 之前返回结构化错误。默认值只服务于当前单机离线知识库，真实部署可按设备内存调整。
struct KnowledgeLoadLimits {
  // 单个 JSONL 文件的最大字节数；超过时拒绝，而不是先读入再检查。
  std::size_t max_file_bytes = 16U * 1024U * 1024U;
  // 最多接受的知识项数。
  std::size_t max_items = 100000;
  // 单条 id 的最大 UTF-8 字节数；id 是去重与同分排序键，必须保持短小可控。
  std::size_t max_id_bytes = 255;
  // 单条 text 的最大 UTF-8 字节数。
  std::size_t max_text_bytes = 64U * 1024U;
};

// 加载成功后的不可变知识库快照。format_version 是字段格式版本，content_fingerprint 是
// 由格式版本与规范化条目内容计算的 FNV-1a 64 指纹；index_version 把两者组合成可写入
// 证据的稳定标识。fingerprint 不承诺是密码学摘要，只用于发现配置漂移和复现问题。
struct KnowledgeBase {
  // 按文件中的有效条目顺序保存；顺序是 index_version 的一部分，但排序结果仍由检索器
  // 重新按分数和 id 决定，不依赖输入行序。
  std::vector<KnowledgeItem> items;
  std::string format_version = kKnowledgeFormatVersion;
  std::string content_fingerprint;

  // 组合版本与内容指纹。空指纹表示调用方手工构造了未加载的 KnowledgeBase；Bm25Rag
  // 仍可工作，但证据应优先使用从文件加载的实例。
  std::string index_version() const;
};

// 从 JSONL 路径加载知识库。成功返回完整快照；失败不含可消费值，并满足以下确定性映射：
//   - 路径为空、JSON 非法、未知字段、字段为空、重复 id、空知识库或超出限制：
//     kInvalidInput，消息包含行号或超限项；
//   - 缺少 id/text 字段：kMissingField，消息包含行号；
//   - 文件不存在或不可读：kDeviceFailure。
// 读取完成后立即关闭文件；函数不写入文件，也不保留路径引用。
domain::Result<KnowledgeBase> load_knowledge_jsonl(
    const std::string& path,
    const KnowledgeLoadLimits& limits = KnowledgeLoadLimits{});

// 对文本做确定性的检索规范化：全角 ASCII 折叠为半角、ASCII 字母转小写、连续空白折叠为
// 一个普通空格、不是字母/数字/CJK 的码点作为边界丢弃。输入按 UTF-8 解码；非法字节序列
// 按每个非法字节作为一个边界处理，不抛异常。该函数是公开规则说明的一部分，调用方可用
// 它解释为何某些查询没有命中。
std::string normalize_search_text(const std::string& text);

// 对规范化后的文本分词。规则：CJK 基本区每个码点一个 token；ASCII 字母/数字连续段整体
// 一个 token 并转小写；其他码点是边界。返回值不保证已规范化空白，但 token 本身不含空白。
std::vector<std::string> tokenize_search_text(const std::string& text);

// BM25 参数与查询边界。k1 > 0 控制词频饱和，b 在 [0,1] 控制文档长度归一化强度；
// max_query_bytes 是单次 query 的 UTF-8 字节上限，超限返回 kInvalidInput 而不是截断。
struct Bm25Options {
  double k1 = 1.2;
  double b = 0.6;
  std::size_t max_query_bytes = 4096;
};

// 基于固定 KnowledgeBase 的只读 BM25 检索器。构造时完成校验、分词和倒排统计；构造后
// 不再访问文件，retrieve 是只读的确定性调用。query 为空或无 token 时返回 kInvalidInput，
// top_k=0 返回成功空列表；无命中也是成功空列表。结果按“分数降序、同分 id 字典序升序”
// 排序，再截断到 top_k。id 唯一由构造校验保证，因此该排序是全序。
class Bm25Rag final : public capability::IRag {
 public:
  // knowledge 为空、条目 id/text 为空、id 重复、文本无法产生 token、options 非法时抛
  // std::invalid_argument。构造不创建外部资源；知识库与选项都按值复制/移动进对象。
  explicit Bm25Rag(KnowledgeBase knowledge, Bm25Options options = {});
  Bm25Rag(const Bm25Rag&) = delete;
  Bm25Rag& operator=(const Bm25Rag&) = delete;
  Bm25Rag(Bm25Rag&&) = default;
  Bm25Rag& operator=(Bm25Rag&&) = default;

  // 返回按分数降序、同分 id 升序的至多 top_k 条命中。失败不产生可消费值；调用方拥有
  // 返回文本副本，本对象不保留 query 引用。同步、无阻塞、无 deadline。
  domain::Result<std::vector<capability::RetrievedChunk>> retrieve(
      const std::string& query, std::size_t top_k) override;

  // 供证据与排错使用；返回值在对象生命周期内稳定。
  const KnowledgeBase& knowledge() const noexcept { return knowledge_; }
  const Bm25Options& options() const noexcept { return options_; }
  std::string index_version() const { return knowledge_.index_version(); }
  const char* tokenization_version() const noexcept { return kSearchTokenizationVersion; }
  std::size_t document_count() const noexcept { return documents_.size(); }

 private:
  double score_document(const std::vector<std::string>& query_tokens,
                        std::size_t document_index) const;

  KnowledgeBase knowledge_;
  Bm25Options options_;
  std::vector<std::vector<std::string>> documents_;
  std::vector<std::unordered_map<std::string, std::size_t>> document_term_frequencies_;
  std::unordered_map<std::string, std::size_t> document_frequency_;
  double average_document_tokens_ = 0.0;
};

}  // namespace nexweave::backend

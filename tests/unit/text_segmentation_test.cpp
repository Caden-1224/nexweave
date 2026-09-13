#include "../test_support.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include "text_segmentation.hpp"

using namespace nexweave;
using nexweave::runtime::is_valid_utf8;
using nexweave::runtime::TextChunk;
using nexweave::runtime::TextChunkReason;
using nexweave::runtime::TextChunker;

namespace {

// 全角句末标点的 UTF-8 字面量。测试直接写字符而不是拼转义序列，使“标点归属前一句”
// 这类断言读起来就是被测文本本身。
constexpr char kFullStop[] = "。";
constexpr char kExclamation[] = "！";
constexpr char kQuestion[] = "？";

// 把片段文本按顺序拼成一个字符串。顺序类断言同时使用“拼接结果”与“片段边界”两条证据：
// 前者证明内容不丢，后者证明切分位置正确。
std::string Join(const std::vector<TextChunk>& chunks) {
  std::string joined;
  for (const auto& chunk : chunks) {
    joined += chunk.text;
  }
  return joined;
}

// 依次送入多段增量文本并返回本次夹具的全部片段。返回值是新的容器，因此调用方不必
// 担心上一次夹具的片段混进本次断言——片段证据只在同一次调用内有效。
std::vector<TextChunk> Feed(TextChunker& chunker, const std::vector<std::string>& tokens) {
  std::vector<TextChunk> chunks;
  for (const auto& token : tokens) {
    chunker.feed(token, chunks);
  }
  return chunks;
}

// 片段文本清单，便于逐项核对切分位置。
std::vector<std::string> Texts(const std::vector<TextChunk>& chunks) {
  std::vector<std::string> texts;
  for (const auto& chunk : chunks) {
    texts.push_back(chunk.text);
  }
  return texts;
}

// 分句不变量：句末标点触发切分且标点归属前一句；非句末标点不切分；token 边界与句子
// 边界无关，因此分句结果只由文本内容决定。用例同时断言“切了几段”与“每段内容”，避免
// 只验证段数而漏掉“标点被推到下一段”这种半对的情况。
void TestTerminatorTriggersChunkAndOwnsPunctuation() {
  TextChunker chunker(60);
  auto chunks = Feed(chunker, {"你好", "，世界", kFullStop, "再见"});
  CHECK(chunks.size() == 1);
  CHECK(chunks.at(0).text == std::string("你好，世界") + kFullStop);
  CHECK(chunks.at(0).reason == TextChunkReason::kTerminator);
  // 未成句的尾部仍留在缓冲里，只有 flush 才交付：提前交付会把半句话送去合成。
  CHECK(chunker.buffered_bytes() == std::string("再见").size());

  chunker.flush(chunks);
  CHECK(chunks.size() == 2);
  CHECK(chunks.at(1).text == "再见");
  CHECK(chunks.at(1).reason == TextChunkReason::kFlushed);
  // 刷新之后缓冲为空且操作幂等：再次刷新不会重复交付同一段回答。
  CHECK(chunker.buffered_bytes() == 0);
  CHECK(chunker.incomplete_utf8() == false);
  chunker.flush(chunks);
  CHECK(chunks.size() == 2);

  // 半角标点与换行符同样切分，但换行符不进入文本：它只表达换行，不是可发音内容。
  // 标点前的空格属于句子内容，因此保留在前一片段里。
  TextChunker ascii(60);
  const auto ascii_chunks = Feed(ascii, {"first!", " second?", "\nthird"});
  CHECK(Texts(ascii_chunks) == std::vector<std::string>({"first!", " second?"}));
  CHECK(ascii_chunks.at(0).reason == TextChunkReason::kTerminator);
  CHECK(ascii.buffered_bytes() == std::string("third").size());

  // 非句末标点不切分：逗号与顿号都只是普通字节，直到句末标点到来才交付。
  TextChunker comma(60);
  const auto comma_chunks = Feed(comma, {"甲、乙，丙"});
  CHECK(comma_chunks.empty());
  CHECK(comma.buffered_bytes() == std::string("甲、乙，丙").size());
}

// 归一化不变量：连续句末标点只保留首个，且任何片段都不以标点开头。它保护下游“一次合成
// 必须是非空可发音文本”的前提：若连续标点各自成段，就会产生只含标点的空句；若片段以
// 标点开头，说明标点被错误地推到了下一段。
void TestConsecutiveAndLeadingPunctuationIsDropped() {
  TextChunker chunker(60);
  const auto chunks =
      Feed(chunker, {"第一句", kFullStop, kExclamation, kQuestion, "第二句", kFullStop});
  CHECK(Texts(chunks) == std::vector<std::string>({std::string("第一句") + kFullStop,
                                                   std::string("第二句") + kFullStop}));
  for (const auto& chunk : chunks) {
    CHECK(chunk.reason == TextChunkReason::kTerminator);
    // 片段开头必须是内容字符：0xE3/0xEF 是中文标点的 UTF-8 前导字节。
    const auto first = static_cast<unsigned char>(chunk.text.front());
    CHECK(first != 0xE3U);
    CHECK(first != 0xEFU);
  }

  // 生成一开始就送标点：在没有任何内容时整体丢弃，不产生空句。
  TextChunker leading(60);
  auto leading_chunks = Feed(leading, {kFullStop, kExclamation, "甲"});
  CHECK(leading_chunks.empty());
  CHECK(leading.buffered_bytes() == std::string("甲").size());

  // 只有标点、没有实质内容时刷新同样不交付：否则会把一段不可发音的噪声送去合成。
  leading.flush(leading_chunks);
  CHECK(leading_chunks.size() == 1);
  CHECK(leading_chunks.at(0).text == "甲");
}

// 长度上限不变量：无标点的长文本必须按字节上限切开，否则合成器要等到整段生成结束才能
// 开始，重叠能力退化为串行。用例同时核对“每段都不超上限”与“拼接结果等于原文”，证明
// 按上限切分不丢字节。
void TestByteLimitSplitsTextWithoutPunctuation() {
  const std::string long_text = "abcdefghijklmnopqrstuvwxyz0123456789";  // 36 字节
  TextChunker chunker(12);
  const auto chunks = Feed(chunker, {long_text});
  CHECK(chunks.size() == 3);
  CHECK(chunks.at(0).reason == TextChunkReason::kMaxBytes);
  CHECK(chunks.at(1).reason == TextChunkReason::kMaxBytes);
  CHECK(chunks.at(2).reason == TextChunkReason::kMaxBytes);
  CHECK(chunks.at(0).text.size() == 12);
  CHECK(chunks.at(1).text.size() == 12);
  CHECK(chunks.at(2).text.size() == 12);
  CHECK(Join(chunks) == long_text);
  CHECK(chunker.buffered_bytes() == 0);

  // 上限与标点同时出现：容量只按字节边界切开，标点始终归属有内容的那一段。上限为 8 时
  // “abcd”与“efgh”合成一段恰好 8 字节并交付，随后“ij。”自成一段，“k”留在缓冲里。
  TextChunker mixed(8);
  const auto mixed_chunks = Feed(mixed, {"abcd", "efgh", "ij", kFullStop, "k"});
  CHECK(Texts(mixed_chunks) ==
        std::vector<std::string>({"abcdefgh", std::string("ij") + kFullStop}));
  CHECK(mixed_chunks.at(0).reason == TextChunkReason::kMaxBytes);
  CHECK(mixed_chunks.at(1).reason == TextChunkReason::kTerminator);
  CHECK(Join(mixed_chunks) == std::string("abcdefghij") + kFullStop);
  for (const auto& chunk : mixed_chunks) {
    CHECK(chunk.text.size() <= 8);
  }
  CHECK(mixed.buffered_bytes() == 1);

  // 标点在还有余量时会被并入本片段（上限 6，缓冲已有 5 字节时“defgh！”整段交付），
  // 而不是被推到下一片段开头。
  TextChunker roomy(6);
  const auto roomy_chunks = Feed(roomy, {"abc", kFullStop, "defgh", kExclamation});
  CHECK(Texts(roomy_chunks) ==
        std::vector<std::string>({std::string("abc") + kFullStop,
                                  std::string("defgh") + kExclamation}));

  // 取舍：文本正好在上限处被切开时，紧跟其后的句末标点会被丢弃。这条断言把“按上限切分
  // 优先于保留标点”的取舍固定下来，避免以后有人把它当成缺陷而反向修改契约。
  TextChunker cut(6);
  const auto cut_chunks = Feed(cut, {"abc", "def", kExclamation, "ghi"});
  CHECK(Texts(cut_chunks) == std::vector<std::string>({"abcdef"}));
  CHECK(cut_chunks.at(0).reason == TextChunkReason::kMaxBytes);
  CHECK(Join(cut_chunks) == "abcdef");
  CHECK(cut.buffered_bytes() == std::string("ghi").size());

  // 上限为 0 表示只按标点切分：这是受控测试专用的宽松模式，长文本不会按字节切开。
  TextChunker unlimited(0);
  const auto unlimited_chunks = Feed(unlimited, {long_text});
  CHECK(unlimited_chunks.empty());
  CHECK(unlimited.buffered_bytes() == long_text.size());
}

// UTF-8 边界不变量：切分点永远落在字符边界上。多字节字符被拆到两个片段会产生非法字节
// 序列，下游合成器要么报错要么静默替换成乱码，两者都会让“回答内容”与“生成内容”不一致。
void TestMultibyteCharacterIsNeverSplitAcrossChunks() {
  // 3 字节字符被 token 边界切开：第一个 token 以半个字符结尾，此时不得切出片段。
  const std::string han = "好";  // E5 A5 BD
  TextChunker chunker(60);
  std::vector<TextChunk> chunks;
  chunker.feed(han.substr(0, 1), chunks);
  CHECK(chunks.empty());
  CHECK(chunker.buffered_bytes() == 1);
  CHECK(chunker.incomplete_utf8());  // 停在半个字符上是正常中间状态，不是错误
  chunker.feed(han.substr(1), chunks);
  CHECK(chunks.empty());
  CHECK(chunker.incomplete_utf8() == false);

  // 容量边界上的“整字符”判定（上限 5，缓冲已有 “ab”，下一个字符是 3 字节汉字）：
  // 2+3 恰好等于上限，因此该字符并入本片段并立即交付，而不是拆成“ab”+半个字符。
  TextChunker exact(5);
  const auto exact_chunks = Feed(exact, {"ab", han});
  CHECK(Texts(exact_chunks) == std::vector<std::string>({std::string("ab") + han}));
  CHECK(exact_chunks.at(0).reason == TextChunkReason::kMaxBytes);
  CHECK(exact.buffered_bytes() == 0);
  for (const auto& item : exact_chunks) {
    CHECK(is_valid_utf8(item.text));
  }

  // 放不下时整个字符被推到下一片段（上限 4，2+3 > 4）：先交付 “ab”，汉字留下一次切分。
  TextChunker narrow(4);
  const auto narrow_chunks = Feed(narrow, {"ab", han});
  CHECK(Texts(narrow_chunks) == std::vector<std::string>({"ab"}));
  CHECK(narrow_chunks.at(0).reason == TextChunkReason::kMaxBytes);
  CHECK(narrow.buffered_bytes() == han.size());
  CHECK(narrow.buffered_bytes() <= narrow.max_chunk_bytes() + 3);

  // 单字符长度超过上限时（上限 2 < 3 字节汉字）必须交付完整字符而不是切碎：这种配置在
  // 真实部署中不会被采用，但契约必须明确它不会产出非法序列。
  TextChunker tiny(2);
  const auto tiny_chunks = Feed(tiny, {han});
  CHECK(Texts(tiny_chunks) == std::vector<std::string>({han}));
  CHECK(tiny.buffered_bytes() == 0);
  for (const auto& item : tiny_chunks) {
    CHECK(is_valid_utf8(item.text));
  }
}

// 收尾不变量：生成正常结束时刷新尾段，片段不遗漏；缓冲为空时刷新是空操作，重复刷新
// 不会重复交付同一段回答。
void TestFlushDeliversTailAndIsIdempotent() {
  TextChunker chunker(60);
  std::vector<TextChunk> chunks;
  // 未成句的尾段留在缓冲里，此时不交付。
  chunker.feed("完整一句", chunks);
  CHECK(chunks.empty());
  CHECK(chunker.buffered_bytes() == std::string("完整一句").size());
  // 刷新交付尾段并清空缓冲；再刷新一次不会重复交付。
  chunker.flush(chunks);
  CHECK(Texts(chunks) == std::vector<std::string>({"完整一句"}));
  CHECK(chunks.at(0).reason == TextChunkReason::kFlushed);
  CHECK(chunker.buffered_bytes() == 0);
  chunker.flush(chunks);
  CHECK(chunks.size() == 1);

  // 缓冲本来就为空时刷新也是空操作。
  TextChunker empty(60);
  std::vector<TextChunk> empty_chunks;
  empty.flush(empty_chunks);
  CHECK(empty_chunks.empty());
}

// 完整性不变量：生成文本不是完整 UTF-8 时不刷新、也不静默丢弃半截字节，而是让调用方
// 通过 incomplete_utf8() 察觉并拒绝本轮；补齐之后同一缓冲可以正常交付。
void TestIncompleteUtf8IsReportedNotSwallowed() {
  TextChunker broken(60);
  std::vector<TextChunk> chunks;
  broken.feed("\xE5\xA5", chunks);  // “好”只到第二个字节
  CHECK(chunks.empty());
  CHECK(broken.incomplete_utf8());
  CHECK(broken.buffered_bytes() == 2);
  // 不完整时不交付：把半个字符送给合成器会让回答内容与生成内容不一致。
  broken.flush(chunks);
  CHECK(chunks.empty());
  CHECK(broken.buffered_bytes() == 2);
  // 补齐后才成为合法字符，此时刷新正常交付。
  broken.feed("\xBD", chunks);
  CHECK(broken.incomplete_utf8() == false);
  broken.flush(chunks);
  CHECK(chunks.size() == 1);
  CHECK(chunks.at(0).text == "好");
  CHECK(chunks.at(0).reason == TextChunkReason::kFlushed);
  CHECK(broken.buffered_bytes() == 0);
}

// UTF-8 合法性判定的边界：空串合法；过长编码、代理区与超过 U+10FFFF 都不合法。下游
// 用它决定是否拒绝一段文本，因此边界必须与标准一致，而不是“看起来像中文就放行”。
void TestUtf8ValidityBoundaries() {
  CHECK(is_valid_utf8(""));
  CHECK(is_valid_utf8("你好，world"));
  CHECK(is_valid_utf8("\xF0\x9F\x98\x80"));  // U+1F600，4 字节合法字符
  CHECK(!is_valid_utf8("\xC0\xAF"));          // 过长编码的斜杠
  CHECK(!is_valid_utf8("\xED\xA0\x80"));      // UTF-16 代理区
  CHECK(!is_valid_utf8("\xF4\x90\x80\x80"));  // 超过 U+10FFFF
  CHECK(!is_valid_utf8("\xE5\xA5"));          // 截断字符
  CHECK(!is_valid_utf8("\x80"));              // 孤立续字节
}

// 计数与有界性不变量：emitted_chunks() 只统计已交付片段（不含缓冲），buffered_bytes()
// 只统计未交付字节，任何时刻“已交付字节 + 缓冲字节”都不超过已送入字节——被丢弃的只有
// 标点与换行符，实质字节不会消失。这条关系让“片段有序、不重复、不遗漏”可以被外部复算，
// 而不是只能相信实现。
void TestCountersAndBufferBoundsStayConsistent() {
  TextChunker chunker(6);
  std::vector<TextChunk> chunks;
  std::size_t fed_bytes = 0;
  const std::vector<std::string> tokens = {"abc", kFullStop, "de", "fgh", kExclamation, "ij"};
  for (const auto& token : tokens) {
    fed_bytes += token.size();
    chunker.feed(token, chunks);
    std::size_t delivered_bytes = 0;
    for (const auto& chunk : chunks) {
      delivered_bytes += chunk.text.size();
    }
    CHECK(delivered_bytes + chunker.buffered_bytes() <= fed_bytes);
    // 缓冲字节数在任何时刻都不超过“上限 + 一个 UTF-8 字符的最大长度”。
    CHECK(chunker.buffered_bytes() <= chunker.max_chunk_bytes() + 3);
    CHECK(chunker.emitted_chunks() == chunks.size());
  }
  // “abc。”达到上限而交付，“defgh！”在下限内把标点并入本段后交付，“ij”留在缓冲里等刷新。
  // 第三条断言特意保留“标点可以并入未达上限的片段”这一事实：丢弃只发生在容量切分之后。
  CHECK(chunker.emitted_chunks() == 2);
  CHECK(chunker.buffered_bytes() == std::string("ij").size());
  chunker.flush(chunks);
  CHECK(chunker.emitted_chunks() == 3);
  CHECK(chunker.buffered_bytes() == 0);
  CHECK(Texts(chunks) ==
        std::vector<std::string>(
            {std::string("abc") + kFullStop, std::string("defgh") + kExclamation, "ij"}));
}

}  // namespace

int main() {
  TestTerminatorTriggersChunkAndOwnsPunctuation();
  TestConsecutiveAndLeadingPunctuationIsDropped();
  TestByteLimitSplitsTextWithoutPunctuation();
  TestMultibyteCharacterIsNeverSplitAcrossChunks();
  TestFlushDeliversTailAndIsIdempotent();
  TestIncompleteUtf8IsReportedNotSwallowed();
  TestUtf8ValidityBoundaries();
  TestCountersAndBufferBoundsStayConsistent();
  return 0;
}

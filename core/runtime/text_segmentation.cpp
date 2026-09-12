#include "text_segmentation.hpp"

#include <cstddef>
#include <utility>

namespace nexweave::runtime {
namespace {

// 是否是 UTF-8 续字节（0b10xxxxxx）。续字节不能出现在片段末尾，否则该片段以半个字符收尾。
bool is_continuation_byte(unsigned char byte) noexcept {
  return (byte & 0xC0U) == 0x80U;
}

// 句末标点识别结果。width 为 0 表示该位置不是句末标点；consume_only 为真表示该标点
// 只用于切分、不进入文本（换行符）。
struct TerminatorMatch {
  std::size_t width = 0;
  bool consume_only = false;
};

// 识别 text[index] 处是否是句末标点。中文标点固定是 UTF-8 三字节序列，按字节模式匹配，
// 因此不需要解码整个字符串，也不会因为解码失败而误判。
// 说明：分号、感叹号、问号的半角与全角形式都算句末标点；逗号、顿号、冒号不算。
TerminatorMatch match_terminator(const std::string& text, std::size_t index) noexcept {
  const auto first = static_cast<unsigned char>(text[index]);
  if (first == '\n') {
    return TerminatorMatch{1, true};
  }
  if (first == '!' || first == '?' || first == ';') {
    return TerminatorMatch{1, false};
  }
  if (text.size() - index < 3) {
    return TerminatorMatch{};
  }
  const auto second = static_cast<unsigned char>(text[index + 1]);
  const auto third = static_cast<unsigned char>(text[index + 2]);
  // U+3002 。，以及 U+FF01 ！/ U+FF1F ？/ U+FF1B ；。
  if (first == 0xE3 && second == 0x80 && third == 0x82) {
    return TerminatorMatch{3, false};
  }
  if (first == 0xEF && second == 0xBC &&
      (third == 0x81 || third == 0x9F || third == 0x9B)) {
    return TerminatorMatch{3, false};
  }
  return TerminatorMatch{};
}

// 按前导字节推导本次待追加的 UTF-8 序列宽度（1..4）。这里只做宽松的长度推导：
// 真正的合法性由 is_valid_utf8 在收尾时统一判定，分句过程只需要知道“不要切在字符中间”。
std::size_t utf8_width(unsigned char first) noexcept {
  if ((first & 0x80U) == 0U) {
    return 1;
  }
  if ((first & 0xE0U) == 0xC0U) {
    return 2;
  }
  if ((first & 0xF0U) == 0xE0U) {
    return 3;
  }
  if ((first & 0xF8U) == 0xF0U) {
    return 4;
  }
  // 非法前导字节（续字节或 0xF8..0xFF）：按单字节推进，把它交给收尾时的合法性检查，
  // 而不是在这里猜测它的长度并可能越界读取。
  return 1;
}

}  // namespace

TextChunker::TextChunker(std::size_t max_chunk_bytes) noexcept
    : max_chunk_bytes_(max_chunk_bytes) {}

void TextChunker::feed(const std::string& text, std::vector<TextChunk>& emitted) {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto terminator = match_terminator(text, index);

    if (terminator.width > 0) {
      // 刚切过一片段（句末标点切分或容量切分），或当前位置之前没有任何内容：该标点属于
      // “句首标点”，直接丢弃，否则会产出只含标点的空片段，让合成器白白起一轮。这同时
      // 实现了“连续句末标点只保留首个”和“片段不以标点开头”两条规则。
      // 代价必须明确：一段文本正好在上限处被切开时，紧跟其后的句末标点会被丢弃，因此
      // “按上限切分”与“标点必然保留”不可兼得；本版选择保留“无标点片段”这条更强的
      // 结构不变量，并在契约里显式记录该取舍。
      if (split_pending_ || buffer_.empty()) {
        split_pending_ = true;
        index += terminator.width;
        continue;
      }
      if (!terminator.consume_only) {
        buffer_ += text.substr(index, terminator.width);
      }
      emit(TextChunkReason::kTerminator, emitted);
      split_pending_ = true;
      index += terminator.width;
      continue;
    }

    const std::size_t width = utf8_width(static_cast<unsigned char>(text[index]));
    if (index + width > text.size()) {
      // 本次 feed 在字符中间结束：把已到达的字节先收进缓冲，等下次 feed 补齐再切，
      // 保证任何片段都不会包含半个多字节字符。
      buffer_.append(text, index, text.size() - index);
      split_pending_ = false;
      index = text.size();
      continue;
    }
    // 容量判定放在追加之前：一旦加上这个字符就会超限，就先把已有内容交付，再放新字符。
    // 因此任何片段都不会超过上限，切分点也永远落在字符边界上。这里不改 split_pending_：
    // 它的值由“追加之后缓冲里有没有内容”统一决定，见下面的赋值。
    if (max_chunk_bytes_ > 0 && !buffer_.empty() &&
        buffer_.size() + width > max_chunk_bytes_) {
      emit(TextChunkReason::kMaxBytes, emitted);
    }
    buffer_.append(text, index, width);
    // 追加了实质字符，说明下一片段的开头已经有了内容；紧随其后的句末标点因此不再是
    // “句首标点”，必须归属本片段。这一赋值必须在容量判定之后执行：若在它之前就写死为
    // false，就会覆盖“刚在缓冲末尾切开”这一事实，使接下来的标点被误判成句首标点而丢弃。
    split_pending_ = false;
    index += width;
    // 达到上限立即交付，而不是等下一个字符到来才发现“已经装不下”。差别在尾部：
    // 若推迟到下一次 feed，一段正好填满上限的文本会一直留在缓冲里，它的延迟就取决于
    // 下一次生成何时发生，而“按上限切分以恢复重叠”的目的恰恰是消除这种依赖。
    if (max_chunk_bytes_ > 0 && buffer_.size() >= max_chunk_bytes_) {
      emit(TextChunkReason::kMaxBytes, emitted);
      // 刚刚在缓冲末尾切开：紧随其后的句末标点属于“句首标点”，必须丢弃，否则会产出
      // 只含标点的空片段。这一步必须在追加与上一条赋值之后执行，否则会被“追加实质字符”
      // 那一步覆盖，使标点被错误地当成新片段的内容。
      split_pending_ = true;
    }
  }
}

void TextChunker::flush(std::vector<TextChunk>& emitted) {
  if (buffer_.empty()) {
    return;
  }
  if (incomplete_utf8()) {
    // 尾段停在半个字符上：不交付、也不清空，让调用方通过 incomplete_utf8() 察觉并
    // 拒绝本轮。悄悄丢弃这半截字节会让“文本完整性”从证据里消失。
    return;
  }
  emit(TextChunkReason::kFlushed, emitted);
}

std::size_t TextChunker::buffered_bytes() const noexcept {
  return buffer_.size();
}

std::size_t TextChunker::emitted_chunks() const noexcept {
  return emitted_chunks_;
}

bool TextChunker::incomplete_utf8() const noexcept {
  // 判定必须走完整的 UTF-8 校验，不能只看“末尾字节是不是续字节”：合法的多字节字符也
  // 经常以续字节收尾（例如“见”= E8 A7 81），只看末字节会把完整文本误判成截断，从而
  // 在收尾时拒绝一条正常回答。校验是一次线性扫描，代价与缓冲上限同阶。
  return !buffer_.empty() && !is_valid_utf8(buffer_);
}

std::size_t TextChunker::max_chunk_bytes() const noexcept {
  return max_chunk_bytes_;
}

void TextChunker::emit(TextChunkReason reason, std::vector<TextChunk>& emitted) {
  TextChunk chunk;
  chunk.text = std::move(buffer_);
  chunk.reason = reason;
  // move 之后 buffer_ 处于有效但未指定的状态，显式清空以保证后续 append 语义明确。
  buffer_.clear();
  ++emitted_chunks_;
  emitted.push_back(std::move(chunk));
}

bool is_valid_utf8(const std::string& text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto first = static_cast<unsigned char>(text[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }
    std::size_t width = 0;
    unsigned char lower = 0x80U;
    unsigned char upper = 0xBFU;
    if (first >= 0xC2U && first <= 0xDFU) {
      width = 2;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      width = 3;
      // 排除过长编码（E0 80..9F）与 UTF-16 代理区（ED A0..BF）。
      if (first == 0xE0U) {
        lower = 0xA0U;
      } else if (first == 0xEDU) {
        upper = 0x9FU;
      }
    } else if (first >= 0xF0U && first <= 0xF4U) {
      width = 4;
      // 排除过长编码（F0 80..8F）与超过 U+10FFFF 的码位（F4 90..BF）。
      if (first == 0xF0U) {
        lower = 0x90U;
      } else if (first == 0xF4U) {
        upper = 0x8FU;
      }
    } else {
      // 0x80..0xC1 是续字节或过长编码的前导字节，0xF5..0xFF 永不合法。
      return false;
    }
    if (index + width > text.size()) {
      return false;
    }
    const auto second = static_cast<unsigned char>(text[index + 1]);
    if (second < lower || second > upper) {
      return false;
    }
    for (std::size_t offset = 2; offset < width; ++offset) {
      if (!is_continuation_byte(static_cast<unsigned char>(text[index + offset]))) {
        return false;
      }
    }
    index += width;
  }
  return true;
}

}  // namespace nexweave::runtime

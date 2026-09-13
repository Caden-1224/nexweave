// NDJSON 增量解帧的单元夹具：验收“字节流 → 完整帧”这一步在任意切分下都稳定。
//
// 为什么单独测这一层：TCP 与 ZeroMQ 都是字节流，不保留发送方的写入边界，一次读取可能返回
// 半帧、数帧或零字节。帧边界一旦判错，后面所有协议校验都会在错误的输入上运行，因此“切分
// 无关”必须是这一层自己的不变量，而不是靠上层容错掩盖。
//
// 保护的不变量（每条在断言旁注明）：
//   1. 切分无关：同一段字节按任意方式切开（逐字节、按帧、混合）得到同一串帧；未成帧的尾部
//      只会留在半包缓冲里，不会被当成帧交付，也不会丢失。
//   2. 边界闭合：只有换行符结束一帧，且换行符本身不属于帧内容。
//   3. 行尾兼容与空行：'\r\n' 的 '\r' 被剥离；空行与仅含 '\r' 的行不产生帧。
//   4. 有界：缓冲不会超过声明上限，也不会为了找换行符而无限增长；超限帧被丢弃、计入计数，
//      并在下一个换行符处重新同步，绝不把超长帧的尾部当成新帧。
//   5. 上限语义含边界：恰好等于上限的帧合法，多一个字节即超限。
//   6. 复位：reset() 回到构造状态，半包、丢弃状态与计数一并清空。
#include "../test_support.hpp"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "ndjson_framer.hpp"

using nexweave::gateway::FrameVerdict;
using nexweave::gateway::NdjsonFramer;

namespace {

// 一次 feed 的帧产出；调用方负责保留 framer，以便随后断言半包与计数。
std::vector<std::string> FeedFrames(NdjsonFramer& framer, const std::string& chunk) {
  std::vector<std::string> frames;
  framer.feed(chunk, frames);
  return frames;
}

// 一帧的完整内容与换行符分开，使“按字节喂入”的循环不必重复拼接。
constexpr char kQueryLine[] = "{\"operation\":\"query\",\"request_id\":\"r-1\"}";

void TestWholeFrameInOneChunk() {
  NdjsonFramer framer;
  const std::vector<std::string> frames = FeedFrames(framer, std::string(kQueryLine) + "\n");
  CHECK(frames.size() == 1);
  CHECK(frames.front() == kQueryLine);
  // 成帧之后不留半包：尾部字节如果留在缓冲里，下一次 feed 会把它拼到新帧前面。
  CHECK(framer.partial_bytes() == 0);
  CHECK(framer.oversized_frames() == 0);
}

void TestByteAtATimeIsSplitIndependent() {
  const std::string line = kQueryLine;
  NdjsonFramer whole;
  const std::vector<std::string> expected = FeedFrames(whole, line + "\n");
  CHECK(expected.size() == 1);

  NdjsonFramer split;
  std::vector<std::string> frames;
  for (const char byte : line) {
    // 半包阶段必须一帧都不产出：把不完整输入当帧交付等于协议层在残缺 JSON 上做决定。
    CHECK(frames.empty());
    split.feed(std::string(1, byte), frames);
  }
  CHECK(frames.empty());
  CHECK(split.partial_bytes() == line.size());
  split.feed("\n", frames);
  CHECK(frames.size() == 1);
  // 逐字节喂入与整帧喂入必须得到同一个帧内容，否则“切分无关”不成立。
  CHECK(frames.front() == expected.front());
  CHECK(split.partial_bytes() == 0);
}

void TestStickyFramesAreSeparated() {
  NdjsonFramer framer;
  const std::vector<std::string> frames = FeedFrames(framer, "a\nbb\nccc\n");
  CHECK(frames.size() == 3);
  CHECK(frames.at(0) == "a");
  CHECK(frames.at(1) == "bb");
  CHECK(frames.at(2) == "ccc");
  CHECK(framer.partial_bytes() == 0);
}

void TestTrailingPartialIsKeptNotDelivered() {
  NdjsonFramer framer;
  const std::vector<std::string> frames = FeedFrames(framer, "a\nbcd");
  CHECK(frames.size() == 1);
  CHECK(frames.front() == "a");
  // 未闭合的尾部只算半包：字节数和内容都留着，等下一次输入补齐。
  CHECK(framer.partial_bytes() == 3);
  const std::vector<std::string> rest = FeedFrames(framer, "e\n");
  CHECK(rest.size() == 1);
  CHECK(rest.front() == "bcde");
}

void TestCarriageReturnAndBlankLines() {
  NdjsonFramer framer;
  const std::vector<std::string> frames = FeedFrames(framer, "a\r\n\r\n\nb\r\n");
  CHECK(frames.size() == 2);
  // '\r' 属于行尾标记而不是帧内容：留下它会让 JSON 解析器面对尾随控制字符。
  CHECK(frames.at(0) == "a");
  CHECK(frames.at(1) == "b");
  CHECK(framer.partial_bytes() == 0);
}

void TestOnlyCarriageReturnLineIsBlank() {
  NdjsonFramer framer;
  // 仅含 '\r' 的一行是空行（Windows 换行下的空行），不产生帧；只含空格的行走正常校验路径。
  const std::vector<std::string> frames = FeedFrames(framer, "\r\n   \n");
  CHECK(frames.size() == 1);
  CHECK(frames.front() == "   ");
}

void TestEmptyChunkChangesNothing() {
  NdjsonFramer framer;
  CHECK(FeedFrames(framer, "").empty());
  CHECK(framer.partial_bytes() == 0);

  CHECK(FeedFrames(framer, "half").empty());
  // 空输入不是“结束帧”：它既不产出帧，也不清掉已经积累的半包。
  CHECK(FeedFrames(framer, "").empty());
  CHECK(framer.partial_bytes() == 4);
  CHECK(FeedFrames(framer, "-more\n").size() == 1);
}

void TestLimitIsInclusiveAtTheBoundary() {
  NdjsonFramer framer(4);
  const std::vector<std::string> exact = FeedFrames(framer, "abcd\n");
  // 恰好等于上限的帧合法：上限约束的是“最多多少字节”，不是“必须小于”。
  CHECK(exact.size() == 1);
  CHECK(exact.front() == "abcd");
  CHECK(framer.oversized_frames() == 0);

  const std::vector<std::string> too_long = FeedFrames(framer, "abcde\n");
  CHECK(too_long.empty());
  CHECK(framer.oversized_frames() == 1);
  CHECK(framer.partial_bytes() == 0);
}

void TestOversizedFrameIsDroppedAndBufferStaysBounded() {
  NdjsonFramer framer(4);
  std::vector<std::string> frames;
  // 没有换行的超长输入：必须在超过上限时立刻放弃，而不是先收下再等换行。
  CHECK(framer.feed("abcde", frames) == FrameVerdict::kOversized);
  CHECK(frames.empty());
  CHECK(framer.oversized_frames() == 1);
  CHECK(framer.partial_bytes() == 0);
}

void TestOversizedFrameResyncsOnNextNewline() {
  NdjsonFramer framer(4);
  // 超长帧与后面的正常帧在同一个批次里：丢弃必须止于换行符，后续帧照常产出。
  std::vector<std::string> frames;
  CHECK(framer.feed("abcde\nok\n", frames) == FrameVerdict::kOversized);
  CHECK(frames.size() == 1);
  CHECK(frames.front() == "ok");
  CHECK(framer.oversized_frames() == 1);
}

void TestOversizedFrameResyncsAcrossChunks() {
  NdjsonFramer framer(4);
  std::vector<std::string> frames;
  CHECK(framer.feed("abcde", frames) == FrameVerdict::kOversized);
  // 超长帧的尾部不是新帧的开头：丢弃模式必须一直吃到换行符为止。
  frames.clear();
  CHECK(framer.feed("fgh\nok\n", frames) == FrameVerdict::kOk);
  CHECK(frames.size() == 1);
  CHECK(frames.front() == "ok");
  CHECK(framer.oversized_frames() == 1);
}

void TestOverflowAcrossPartialChunks() {
  NdjsonFramer framer(4);
  std::vector<std::string> frames;
  CHECK(framer.feed("ab", frames) == FrameVerdict::kOk);
  CHECK(framer.partial_bytes() == 2);
  // 累加超过上限同样要判定超限：半包缓冲不能随连接存活时间无界增长。
  CHECK(framer.feed("cde", frames) == FrameVerdict::kOversized);
  CHECK(frames.empty());
  CHECK(framer.partial_bytes() == 0);
}

void TestResetReturnsToInitialState() {
  NdjsonFramer framer(4);
  std::vector<std::string> frames;
  CHECK(framer.feed("abcde", frames) == FrameVerdict::kOversized);
  CHECK(framer.oversized_frames() == 1);

  framer.reset();
  CHECK(framer.partial_bytes() == 0);
  CHECK(framer.oversized_frames() == 0);
  // 复位后必须能继续正常解帧：丢弃状态如果没被清掉，后续输入会被整段吞掉。
  CHECK(framer.feed("ab\ncd\n", frames) == FrameVerdict::kOk);
  CHECK(frames.size() == 2);
  CHECK(frames.at(0) == "ab");
  CHECK(frames.at(1) == "cd");
}

void TestZeroLimitIsNormalisedToTheSmallestUsableBound() {
  NdjsonFramer framer(0);
  // 上限 0 归一为 1：非法配置退化为“只接受单字节帧”，而不是退化成“不限制长度”。
  CHECK(FeedFrames(framer, "a\n").size() == 1);
  CHECK(FeedFrames(framer, "ab\n").empty());
  CHECK(framer.oversized_frames() == 1);
}

}  // namespace

int main() {
  try {
    TestWholeFrameInOneChunk();
    TestByteAtATimeIsSplitIndependent();
    TestStickyFramesAreSeparated();
    TestTrailingPartialIsKeptNotDelivered();
    TestCarriageReturnAndBlankLines();
    TestOnlyCarriageReturnLineIsBlank();
    TestEmptyChunkChangesNothing();
    TestLimitIsInclusiveAtTheBoundary();
    TestOversizedFrameIsDroppedAndBufferStaysBounded();
    TestOversizedFrameResyncsOnNextNewline();
    TestOversizedFrameResyncsAcrossChunks();
    TestOverflowAcrossPartialChunks();
    TestResetReturnsToInitialState();
    TestZeroLimitIsNormalisedToTheSmallestUsableBound();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "NDJSON 解帧用例未通过: %s\n", error.what());
    return 1;
  }
  return 0;
}

#include "ndjson_framer.hpp"

namespace nexweave::gateway {

NdjsonFramer::NdjsonFramer(std::size_t max_frame_bytes)
    : max_frame_bytes_(max_frame_bytes == 0 ? 1 : max_frame_bytes) {}

void NdjsonFramer::emit_line(std::string& line, std::vector<std::string>& frames) {
  // 只剥离一个行尾 '\r'：把它当作行尾标记而不是内容，同时不动帧内其他位置的回车。
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line.empty()) {
    return;
  }
  frames.push_back(line);
}

FrameVerdict NdjsonFramer::feed(std::string_view chunk, std::vector<std::string>& frames) {
  FrameVerdict verdict = FrameVerdict::kOk;
  std::size_t position = 0;
  while (position < chunk.size()) {
    if (discarding_) {
      // 丢弃模式：整段吞到第一个换行符为止。找不到换行符时本批输入全部丢弃即可，
      // 不需要保留任何字节——它们已经确定属于那个被拒的帧。
      const std::size_t newline = chunk.find('\n', position);
      if (newline == std::string_view::npos) {
        return verdict;
      }
      discarding_ = false;
      position = newline + 1;
      continue;
    }

    const std::size_t newline = chunk.find('\n', position);
    // take 是本行在本批输入里可见的字节数：没有换行符时它就是剩余长度（半包）。
    const std::size_t take =
        (newline == std::string_view::npos ? chunk.size() : newline) - position;
    if (buffer_.size() + take > max_frame_bytes_) {
      // 超限判定在**收下字节之前**：先收再查会让缓冲短暂超过上限，超长输入也就有了
      // 一次性撑大内存的机会。本行若已在本批输入里换行，直接跳过它即可；否则转入丢弃
      // 模式，把它的剩余字节一直吞到下一个换行符。
      buffer_.clear();
      ++oversized_frames_;
      verdict = FrameVerdict::kOversized;
      if (newline == std::string_view::npos) {
        discarding_ = true;
        return verdict;
      }
      position = newline + 1;
      continue;
    }

    buffer_.append(chunk.data() + position, take);
    if (newline == std::string_view::npos) {
      return verdict;
    }
    position = newline + 1;
    emit_line(buffer_, frames);
    buffer_.clear();
  }
  return verdict;
}

std::size_t NdjsonFramer::partial_bytes() const noexcept {
  return buffer_.size();
}

std::uint64_t NdjsonFramer::oversized_frames() const noexcept {
  return oversized_frames_;
}

void NdjsonFramer::reset() noexcept {
  buffer_.clear();
  discarding_ = false;
  oversized_frames_ = 0;
}

}  // namespace nexweave::gateway

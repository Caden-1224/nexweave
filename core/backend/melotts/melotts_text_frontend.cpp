#include "melotts_text_frontend.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace nexweave::backend {
namespace {

constexpr std::size_t kMaxLexiconKeyCodepoints = 32;

struct DecodedCodepoint {
  std::uint32_t value = 0;
  std::size_t bytes = 0;
  bool valid = false;
};

DecodedCodepoint decode_one(const std::string& text, std::size_t offset) {
  DecodedCodepoint result;
  if (offset >= text.size()) {
    return result;
  }
  const unsigned char first = static_cast<unsigned char>(text[offset]);
  if (first < 0x80U) {
    result.value = first;
    result.bytes = 1;
    result.valid = true;
    return result;
  }
  std::size_t length = 0;
  std::uint32_t value = 0;
  if ((first & 0xE0U) == 0xC0U) {
    length = 2;
    value = first & 0x1FU;
  } else if ((first & 0xF0U) == 0xE0U) {
    length = 3;
    value = first & 0x0FU;
  } else if ((first & 0xF8U) == 0xF0U) {
    length = 4;
    value = first & 0x07U;
  } else {
    return result;
  }
  if (offset + length > text.size()) {
    return result;
  }
  for (std::size_t index = 1; index < length; ++index) {
    const unsigned char continuation = static_cast<unsigned char>(text[offset + index]);
    if ((continuation & 0xC0U) != 0x80U) {
      return result;
    }
    value = (value << 6U) | (continuation & 0x3FU);
  }
  result.value = value;
  result.bytes = length;
  result.valid = true;
  return result;
}

std::string encode_one(std::uint32_t codepoint) {
  std::string output;
  if (codepoint <= 0x7FU) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FFU) {
    output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint <= 0xFFFFU) {
    output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint <= 0x10FFFFU) {
    output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
  return output;
}

bool is_ascii_letter(std::uint32_t codepoint) {
  return (codepoint >= 'A' && codepoint <= 'Z') ||
         (codepoint >= 'a' && codepoint <= 'z');
}

bool is_ascii_digit(std::uint32_t codepoint) {
  return codepoint >= '0' && codepoint <= '9';
}

bool is_fullwidth_digit(std::uint32_t codepoint) {
  return codepoint >= 0xFF10U && codepoint <= 0xFF19U;
}

std::uint32_t fullwidth_digit_value(std::uint32_t codepoint) {
  return codepoint - 0xFF10U;
}

std::string chinese_digit(std::uint32_t digit) {
  static const char* const kDigits[] = {
      "零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  if (digit > 9U) {
    return "零";
  }
  return kDigits[digit];
}

std::vector<std::uint32_t> decode_codepoints(const std::string& text) {
  std::vector<std::uint32_t> codepoints;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const DecodedCodepoint decoded = decode_one(text, offset);
    if (!decoded.valid) {
      codepoints.push_back(static_cast<unsigned char>(text[offset]));
      offset += 1;
      continue;
    }
    codepoints.push_back(decoded.value);
    offset += decoded.bytes;
  }
  return codepoints;
}

std::string normalize_for_speech(const std::string& text) {
  const std::vector<std::uint32_t> codepoints = decode_codepoints(text);
  std::string normalized;
  for (std::size_t index = 0; index < codepoints.size(); ++index) {
    const std::uint32_t codepoint = codepoints[index];
    if (is_ascii_digit(codepoint)) {
      normalized += chinese_digit(codepoint - '0');
      continue;
    }
    if (is_fullwidth_digit(codepoint)) {
      normalized += chinese_digit(fullwidth_digit_value(codepoint));
      continue;
    }
    const bool decimal_separator = codepoint == '.' || codepoint == ':';
    if (decimal_separator && index > 0 && index + 1 < codepoints.size() &&
        (is_ascii_digit(codepoints[index - 1]) ||
         is_fullwidth_digit(codepoints[index - 1])) &&
        (is_ascii_digit(codepoints[index + 1]) ||
         is_fullwidth_digit(codepoints[index + 1]))) {
      normalized += "点";
      continue;
    }
    normalized += encode_one(codepoint);
  }
  return normalized;
}

std::string map_punctuation(std::uint32_t codepoint) {
  switch (codepoint) {
    case 0x3002U:  // 。
    case '.':
      return ".";
    case 0xFF01U:  // ！
    case '!':
      return "!";
    case 0xFF1FU:  // ？
    case '?':
      return "?";
    case 0xFF0CU:  // ，
    case 0xFF1BU:  // ；
    case 0x3001U:  // 、
    case 0xFF1AU:  // ：
    case ',':
    case ';':
    case ':':
      return ",";
    case 0x2026U:  // …
      return "…";
    case '\'':
      return "'";
    case '-':
      return "-";
    default:
      return {};
  }
}

bool is_space_codepoint(std::uint32_t codepoint) {
  return codepoint == ' ' || codepoint == '\t' || codepoint == '\n' ||
         codepoint == '\r' || codepoint == 0x3000U;
}

std::string lower_ascii(const std::string& text) {
  std::string output = text;
  std::transform(output.begin(), output.end(), output.begin(), [](unsigned char byte) {
    return static_cast<char>(std::tolower(byte));
  });
  return output;
}

bool parse_integer(const std::string& text, std::int32_t* output) {
  if (text.empty() || output == nullptr) {
    return false;
  }
  try {
    std::size_t consumed = 0;
    const long value = std::stol(text, &consumed, 10);
    if (consumed != text.size() || value < 0 ||
        value > static_cast<long>(std::numeric_limits<std::int32_t>::max())) {
      return false;
    }
    *output = static_cast<std::int32_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

std::vector<std::string> split_whitespace(const std::string& line) {
  std::istringstream stream(line);
  std::vector<std::string> parts;
  std::string part;
  while (stream >> part) {
    parts.push_back(part);
  }
  return parts;
}

std::size_t codepoint_count(const std::string& text) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const DecodedCodepoint decoded = decode_one(text, offset);
    offset += decoded.valid ? decoded.bytes : 1U;
    ++count;
  }
  return count;
}

// 取从 offset 开始、包含 count 个完整 UTF-8 码点的子串。输入越界时返回空。
std::string codepoint_substr(const std::string& text,
                             std::size_t offset,
                             std::size_t count) {
  std::size_t current = offset;
  for (std::size_t index = 0; index < count; ++index) {
    const DecodedCodepoint decoded = decode_one(text, current);
    if (!decoded.valid) {
      return {};
    }
    current += decoded.bytes;
  }
  return text.substr(offset, current - offset);
}

bool contains_ascii(const std::string& text) {
  for (unsigned char byte : text) {
    if (byte < 0x80U) {
      return true;
    }
  }
  return false;
}

}  // namespace

domain::Result<MeloTextFrontend> MeloTextFrontend::create(
    const std::string& lexicon_path,
    const std::string& tokens_path) {
  return load(lexicon_path, tokens_path);
}

domain::Result<MeloTextFrontend> MeloTextFrontend::load(
    const std::string& lexicon_path,
    const std::string& tokens_path) {
  if (lexicon_path.empty() || tokens_path.empty()) {
    return domain::Result<MeloTextFrontend>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 文本前端路径为空");
  }

  std::ifstream tokens_file(tokens_path);
  if (!tokens_file.is_open()) {
    return domain::Result<MeloTextFrontend>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS tokens.txt 不可读");
  }

  MeloTextFrontend frontend;
  std::string line;
  while (std::getline(tokens_file, line)) {
    const std::vector<std::string> parts = split_whitespace(line);
    if (parts.empty()) {
      continue;
    }
    if (parts.size() != 2) {
      return domain::Result<MeloTextFrontend>::failure(
          domain::ErrorCode::kInvalidInput, "MeloTTS tokens.txt 行格式非法");
    }
    std::int32_t id = 0;
    if (!parse_integer(parts[1], &id)) {
      return domain::Result<MeloTextFrontend>::failure(
          domain::ErrorCode::kInvalidInput, "MeloTTS token id 非法");
    }
    frontend.token_ids_[parts[0]] = id;
  }

  const auto blank = frontend.token_ids_.find("_");
  if (blank != frontend.token_ids_.end()) {
    frontend.blank_id_ = blank->second;
  }
  const auto unk = frontend.token_ids_.find("UNK");
  if (unk != frontend.token_ids_.end()) {
    frontend.unk_id_ = unk->second;
  } else {
    return domain::Result<MeloTextFrontend>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS tokens.txt 缺少 UNK");
  }

  std::ifstream lexicon_file(lexicon_path);
  if (!lexicon_file.is_open()) {
    return domain::Result<MeloTextFrontend>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS lexicon.txt 不可读");
  }

  while (std::getline(lexicon_file, line)) {
    const std::vector<std::string> parts = split_whitespace(line);
    if (parts.size() < 3U || ((parts.size() - 1U) % 2U) != 0U) {
      continue;
    }
    const std::size_t phone_count = (parts.size() - 1U) / 2U;
    Unit unit;
    unit.phones.reserve(phone_count);
    unit.tones.reserve(phone_count);
    bool valid = true;
    for (std::size_t index = 0; index < phone_count; ++index) {
      const auto token = frontend.token_ids_.find(parts[index + 1U]);
      if (token == frontend.token_ids_.end()) {
        valid = false;
        break;
      }
      unit.phones.push_back(token->second);
    }
    if (!valid) {
      continue;
    }
    for (std::size_t index = 0; index < phone_count; ++index) {
      std::int32_t tone = 0;
      if (!parse_integer(parts[phone_count + index + 1U], &tone)) {
        valid = false;
        break;
      }
      unit.tones.push_back(tone);
    }
    if (!valid) {
      continue;
    }
    frontend.max_key_codepoints_ =
        std::max(frontend.max_key_codepoints_,
                 std::min(kMaxLexiconKeyCodepoints, codepoint_count(parts[0])));
    frontend.lexicon_[parts[0]] = std::move(unit);
  }

  if (frontend.lexicon_.empty()) {
    return domain::Result<MeloTextFrontend>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS lexicon.txt 没有有效条目");
  }
  frontend.loaded_ = true;
  return domain::Result<MeloTextFrontend>::success(std::move(frontend));
}

domain::Result<std::vector<MeloPhoneChunk>> MeloTextFrontend::convert(
    const std::string& text,
    std::size_t max_encoder_phones) const {
  if (!loaded_) {
    return domain::Result<std::vector<MeloPhoneChunk>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 文本前端未加载");
  }
  if (text.empty()) {
    return domain::Result<std::vector<MeloPhoneChunk>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 输入文本为空");
  }
  if (max_encoder_phones < 3U) {
    return domain::Result<std::vector<MeloPhoneChunk>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS phone 预算非法");
  }

  const std::size_t actual_budget = (max_encoder_phones - 1U) / 2U;
  if (actual_budget == 0U) {
    return domain::Result<std::vector<MeloPhoneChunk>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS phone 预算非法");
  }

  const std::string normalized = normalize_for_speech(text);
  std::vector<Unit> units;
  std::size_t unknown_units = 0;
  std::size_t offset = 0;
  while (offset < normalized.size()) {
    const DecodedCodepoint decoded = decode_one(normalized, offset);
    if (!decoded.valid) {
      ++offset;
      continue;
    }
    const std::uint32_t codepoint = decoded.value;

    if (is_ascii_letter(codepoint)) {
      const std::size_t begin = offset;
      offset += decoded.bytes;
      while (offset < normalized.size()) {
        const DecodedCodepoint next = decode_one(normalized, offset);
        if (!next.valid || !is_ascii_letter(next.value)) {
          break;
        }
        offset += next.bytes;
      }
      const std::string word = lower_ascii(normalized.substr(begin, offset - begin));
      const auto exact = lexicon_.find(word);
      if (exact != lexicon_.end()) {
        units.push_back(exact->second);
        continue;
      }
      for (char byte : word) {
        const std::string letter(1, byte);
        const auto entry = lexicon_.find(letter);
        if (entry != lexicon_.end()) {
          units.push_back(entry->second);
        } else {
          units.push_back(Unit{{unk_id_}, {0}});
          ++unknown_units;
        }
      }
      continue;
    }

    if (is_space_codepoint(codepoint)) {
      offset += decoded.bytes;
      continue;
    }

    const std::string punctuation = map_punctuation(codepoint);
    if (!punctuation.empty()) {
      const auto entry = lexicon_.find(punctuation);
      if (entry != lexicon_.end()) {
        units.push_back(entry->second);
      } else {
        const auto token = token_ids_.find(punctuation);
        if (token != token_ids_.end()) {
          units.push_back(Unit{{token->second}, {0}});
        }
      }
      offset += decoded.bytes;
      continue;
    }

    // 非 ASCII 文本段：在段内做最长词典匹配，使中文词语优先于单字。
    const std::size_t run_begin = offset;
    while (offset < normalized.size()) {
      const DecodedCodepoint next = decode_one(normalized, offset);
      if (!next.valid || is_ascii_letter(next.value) || is_space_codepoint(next.value) ||
          !map_punctuation(next.value).empty() ||
          (next.value < 0x80U && !is_ascii_letter(next.value))) {
        break;
      }
      offset += next.bytes;
    }
    if (offset == run_begin) {
      offset += decoded.bytes;
      units.push_back(Unit{{unk_id_}, {0}});
      ++unknown_units;
      continue;
    }

    std::size_t current = run_begin;
    while (current < offset) {
      bool matched = false;
      const std::size_t remaining = codepoint_count(normalized.substr(
          current, offset - current));
      const std::size_t max_length = std::min(max_key_codepoints_, remaining);
      for (std::size_t length = max_length; length >= 1U; --length) {
        const std::string key = codepoint_substr(normalized, current, length);
        if (key.empty() || contains_ascii(key)) {
          continue;
        }
        const auto entry = lexicon_.find(key);
        if (entry != lexicon_.end()) {
          units.push_back(entry->second);
          current += key.size();
          matched = true;
          break;
        }
      }
      if (matched) {
        continue;
      }
      const DecodedCodepoint single = decode_one(normalized, current);
      if (!single.valid) {
        ++current;
        continue;
      }
      const std::string key = normalized.substr(current, single.bytes);
      const auto entry = lexicon_.find(key);
      if (entry != lexicon_.end()) {
        units.push_back(entry->second);
      } else {
        units.push_back(Unit{{unk_id_}, {0}});
        ++unknown_units;
      }
      current += single.bytes;
    }
  }

  if (units.empty()) {
    return domain::Result<std::vector<MeloPhoneChunk>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 输入没有可合成内容");
  }

  // 单个 unit 超过预算时只在 phone 序列上切开，保留全部音素；随后按预算贪心打包。
  std::vector<Unit> bounded_units;
  for (const Unit& unit : units) {
    if (unit.phones.empty()) {
      continue;
    }
    std::size_t begin = 0;
    while (begin < unit.phones.size()) {
      const std::size_t count = std::min(actual_budget, unit.phones.size() - begin);
      Unit sub;
      sub.phones.assign(unit.phones.begin() + static_cast<std::ptrdiff_t>(begin),
                        unit.phones.begin() + static_cast<std::ptrdiff_t>(begin + count));
      sub.tones.assign(unit.tones.begin() + static_cast<std::ptrdiff_t>(begin),
                       unit.tones.begin() + static_cast<std::ptrdiff_t>(begin + count));
      bounded_units.push_back(std::move(sub));
      begin += count;
    }
  }

  if (bounded_units.empty()) {
    return domain::Result<std::vector<MeloPhoneChunk>>::failure(
        domain::ErrorCode::kInvalidInput, "MeloTTS 输入没有可合成内容");
  }

  std::vector<MeloPhoneChunk> chunks;
  std::size_t chunk_phone_count = 0;
  std::size_t chunk_unknown = 0;
  std::vector<Unit> chunk_units;
  const auto flush_chunk = [&]() {
    if (chunk_units.empty()) {
      return;
    }
    MeloPhoneChunk chunk;
    std::size_t phone_total = 0;
    for (const Unit& unit : chunk_units) {
      phone_total += unit.phones.size();
    }
    chunk.phones.assign(phone_total * 2U + 1U, blank_id_);
    chunk.tones.assign(phone_total * 2U + 1U, 0);
    chunk.languages.assign(phone_total * 2U + 1U, 3);
    std::size_t phone_index = 0;
    for (const Unit& unit : chunk_units) {
      chunk.word_phone_counts.push_back(unit.phones.size() * 2U);
      for (std::size_t index = 0; index < unit.phones.size(); ++index) {
        const std::size_t target = phone_index * 2U + 1U;
        chunk.phones[target] = unit.phones[index];
        chunk.tones[target] = unit.tones[index];
        ++phone_index;
      }
    }
    if (!chunk.word_phone_counts.empty()) {
      chunk.word_phone_counts[0] += 1U;
    }
    chunk.unknown_units = chunk_unknown;
    chunks.push_back(std::move(chunk));
    chunk_units.clear();
    chunk_phone_count = 0;
    chunk_unknown = 0;
  };

  for (const Unit& unit : bounded_units) {
    if (!chunk_units.empty() && chunk_phone_count + unit.phones.size() > actual_budget) {
      flush_chunk();
    }
    chunk_units.push_back(unit);
    chunk_phone_count += unit.phones.size();
    if (unit.phones.size() == 1U && unit.phones[0] == unk_id_) {
      ++chunk_unknown;
    }
  }
  flush_chunk();

  return domain::Result<std::vector<MeloPhoneChunk>>::success(std::move(chunks));
}

}  // namespace nexweave::backend

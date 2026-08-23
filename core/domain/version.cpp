// version.hpp 的实现。解析顺序刻意设计为：
// 1) 空串先行判定（kEmpty），与格式错误区分；
// 2) 分隔符计数先行（kTooManyParts），避免把“段数过多”与“某段格式错误”混淆；
// 3) 最后逐段解析（kMalformed）。
// 该顺序保证错误码互斥且可预测：同一输入永远返回同一错误码。

#include "version.hpp"

namespace nexweave::domain {
namespace {

// 解析单个分量：必须是纯 ASCII 数字且数值不超过 uint32 上限。
// 任何非数字字符（符号、空白等）或溢出都返回 false；空段也返回 false。
// 不做截断或取模，溢出按格式错误处理，防止版本号静默变小。
bool ParseComponent(std::string_view part, std::uint32_t& out) {
  if (part.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : part) {
    if (c < '0' || c > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    // 溢出预检必须在乘法之前：若先相乘再加，足够长的数字串会先溢出
    // uint64 并回绕成小值（如 2^64 回绕为 0），后续检查将失效。
    // 用除法预检保证结果不超过 uint32 上限，任何超限输入都直接失败。
    if (value > (0xFFFFFFFFull - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

}  // namespace

ParseVersionResult parse_version(std::string_view text) noexcept {
  ParseVersionResult result;

  if (text.empty()) {
    result.error = VersionError::kEmpty;
    return result;
  }

  // 先数分隔符：恰好 2 个点才构成 3 段；超过即返回 kTooManyParts。
  std::size_t dot_count = 0;
  for (const char c : text) {
    if (c == '.') {
      ++dot_count;
    }
  }
  if (dot_count > 2) {
    result.error = VersionError::kTooManyParts;
    return result;
  }

  // 按 '.' 切出最多 3 段；切分用一次线性扫描完成，不分配临时内存。
  // 段数不足（0 或 1 个点）在本函数末尾统一判定，保证“缺失字段”只映射到 kMalformed。
  std::string_view parts[3];
  std::size_t part_index = 0;
  std::size_t begin = 0;
  for (std::size_t i = 0; i <= text.size() && part_index < 3; ++i) {
    if (i == text.size() || text[i] == '.') {
      parts[part_index++] = text.substr(begin, i - begin);
      begin = i + 1;
    }
  }

  if (part_index != 3) {
    result.error = VersionError::kMalformed;
    return result;
  }

  std::uint32_t values[3];
  for (std::size_t i = 0; i < 3; ++i) {
    if (!ParseComponent(parts[i], values[i])) {
      result.error = VersionError::kMalformed;
      return result;
    }
  }

  result.version = Version{values[0], values[1], values[2]};
  return result;
}

}  // namespace nexweave::domain

#include "identifiers.hpp"

#include <limits>

namespace nexweave::domain {
namespace {
bool safe_id(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128) {
    return false;
  }
  for (const unsigned char c : value) {
    if (!(((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ||
          c == '-' || c == '_' || c == '.')) {
      return false;
    }
  }
  return true;
}

bool numeric_suffix(std::string_view value, std::string_view prefix) noexcept {
  if (value.size() <= prefix.size() || value.size() > 128 ||
      value.substr(0, prefix.size()) != prefix) {
    return false;
  }
  for (const unsigned char c : value.substr(prefix.size())) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}
}  // namespace

bool is_valid_work_id(std::string_view value) noexcept {
  return numeric_suffix(value, "w-");
}
bool is_valid_request_id(std::string_view value) noexcept {
  return safe_id(value);
}
bool is_valid_session_id(std::string_view value) noexcept {
  return value.empty() || safe_id(value);
}

std::uint64_t Generation::advance() noexcept {
  if (value_ == std::numeric_limits<std::uint64_t>::max()) {
    return value_;
  }
  return ++value_;
}
}  // namespace nexweave::domain

// 任务、请求、会话和取消代际标识的领域契约。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace nexweave::domain {

bool is_valid_work_id(std::string_view value) noexcept;
bool is_valid_request_id(std::string_view value) noexcept;
bool is_valid_session_id(std::string_view value) noexcept;

// Session 独占此计数器；每次开始新请求或取消都推进一代，旧事件不可接受。
class Generation {
 public:
  std::uint64_t current() const noexcept { return value_; }
  std::uint64_t advance() noexcept;
  bool accepts(std::uint64_t generation) const noexcept {
    return generation == value_;
  }

 private:
  std::uint64_t value_ = 0;
};

}  // namespace nexweave::domain

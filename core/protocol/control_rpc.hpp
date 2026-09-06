#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include "../domain/error.hpp"
namespace nexweave::protocol {
// 控制面请求值对象：版本、关联标识、操作和 deadline 组成一次幂等调用；不拥有 socket/线程。
struct ControlRequest {
  std::uint32_t version = 1;
  std::string request_id;
  std::string operation;
  std::string work_id;
  std::string session_id;
  std::uint64_t generation = 0;
  std::chrono::milliseconds deadline{0};
};
// 响应始终回显 request_id；replayed 表示幂等缓存直接重放，调用方不得再次启动任务。
struct ControlResponse {
  std::uint32_t version = 1;
  std::string request_id;
  domain::OperationResult result{};
  bool replayed = false;
};
domain::OperationResult validate_request(const ControlRequest&) noexcept;
domain::OperationResult validate_response(const ControlResponse&) noexcept;
domain::Result<std::string> encode_request(const ControlRequest&);
domain::Result<ControlRequest> decode_request(std::string_view);
domain::Result<std::string> encode_response(const ControlResponse&);
domain::Result<ControlResponse> decode_response(std::string_view);
}

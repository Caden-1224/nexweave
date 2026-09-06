// JSON 边界的内部工具；不属于后端能力接口，不把 JSON 类型传给业务调用方。
// C++17，nlohmann-json >= 3.10；仅处理内存值，无线程、socket、文件或时钟状态。
// 先检查类型与范围再转换，避免布尔/浮点/有符号值隐式转换绕过版本、错误码和代际约束。
// decode 把格式/字段异常转为 kInvalidInput，分配失败继续抛出 std::bad_alloc；
// 临时 JSON 与值对象由栈释放。调用方拥有输入字符串，工具不保留引用，可并发调用。
#pragma once

#include <initializer_list>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "../domain/error.hpp"

namespace nexweave::protocol::detail {
using Json = nlohmann::json;

// 目前的整数协议字段全部非负；T 限定为非 bool 的整数，目标上限在转换前检查。
// 负数、布尔、小数、超出 T 范围一律失败；uint64 最大值仍可精确往返，不经过 double。
template <class T>
T integer(const Json& json, const char* field) {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
  const auto& value = json.at(field);
  std::uint64_t number = 0;
  if (value.is_number_unsigned()) {
    number = value.get<std::uint64_t>();
  } else if (value.is_number_integer()) {
    const auto signed_number = value.get<std::int64_t>();
    if (signed_number < 0) {
      throw std::invalid_argument(std::string(field) + " 必须非负");
    }
    number = static_cast<std::uint64_t>(signed_number);
  } else {
    throw std::invalid_argument(std::string(field) + " 必须是整数");
  }
  if (number > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
    throw std::invalid_argument(std::string(field) + " 超出字段范围");
  }
  return static_cast<T>(number);
}

// v1 使用固定字段集合：缺失、多余字段或非对象均拒绝；扩展必须显式变更协议版本。
// parse_fields 只在字段集合有效后运行，返回领域校验结果；输入和失败结果均无共享状态。
template <class T, class ParseFields>
domain::Result<T> decode(std::string_view input, std::initializer_list<const char*> fields,
                         ParseFields parse_fields) {
  try {
    const auto json = Json::parse(input.begin(), input.end());
    if (!json.is_object() || json.size() != fields.size()) {
      throw std::invalid_argument("JSON 字段集合不匹配");
    }
    for (const auto* field : fields) {
      if (!json.contains(field)) {
        throw std::invalid_argument(std::string("缺少 JSON 字段: ") + field);
      }
    }
    return parse_fields(json);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    return domain::Result<T>::failure(domain::ErrorCode::kInvalidInput, error.what());
  }
}

// 字符串编码要求 UTF-8；非法文本以结构化失败返回，内存分配异常仍由调用方处理。
// 序列化不改变输入；失败时局部字符串由标准库清理，不返回部分 JSON。
inline domain::Result<std::string> dump(const Json& json) {
  try {
    return domain::Result<std::string>::success(json.dump());
  } catch (const Json::exception& error) {
    return domain::Result<std::string>::failure(domain::ErrorCode::kInvalidInput, error.what());
  }
}
}  // namespace nexweave::protocol::detail

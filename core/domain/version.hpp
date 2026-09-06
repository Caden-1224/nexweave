// NexWeave 领域值：语义版本号（骨架阶段的最小领域模块）。
//
// 职责：统一表示与解析 "主.次.修订" 形式的版本号，供构建元信息、
// 实验证据和后续协议版本协商使用；核心只依赖标准库，保持可移植。
//
// 输入前提：parse_version 接受任意字节序列；空串与非规范格式是合法的失败输入。
// 输出后置：成功时 version 三个分量均有效、error 为 kNone；失败时 version 保持默认值
// （0.0.0）且 error 精确区分“输入缺失 / 格式错误 / 分段过多”。
// 线程安全：本模块无状态，所有函数为纯函数，可任意并发调用，不持有资源。
// 阻塞与 deadline：不适用（纯内存计算，无 I/O、无锁）。
//
// 版本常量与 CMakeLists.txt 中 project(nexweave VERSION ...) 的同步约束：
// 两者必须一致；tests/unit/version_test.cpp 的 TestCmakeVersionConsistency
// 用构建时生成的 nexweave_version_from_cmake.h 校验，不一致即测试失败。

#pragma once

#include <cstdint>
#include <string_view>

namespace nexweave::domain {

// 语义版本号：主版本.次版本.修订版本，各分量为非负 32 位整数。
// 全零（0.0.0）与各分量 uint32 上限均为合法值。
struct Version {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;
};

// 值语义相等：仅比较三个分量，与对象的构造来源无关。
inline bool operator==(const Version& lhs, const Version& rhs) {
  return lhs.major == rhs.major && lhs.minor == rhs.minor && lhs.patch == rhs.patch;
}

// 版本解析错误码。按失败原因分别报告，便于上层把解析失败
// 映射为控制面结构化错误。
enum class VersionError {
  kNone,          // 解析成功，无错误
  kEmpty,         // 输入为空串（输入缺失）
  kMalformed,     // 段数不足（如 "1.2"）或某段含非数字字符 / 数值溢出
  kTooManyParts,  // 分段超过三个（如 "1.2.3.4"）
};

// 解析结果：成功时 version 有效；失败时 error 说明原因且 version 为默认值。
// 不抛出异常，所有失败通过 error 字段显式返回。
struct ParseVersionResult {
  Version version;
  VersionError error = VersionError::kNone;

  // 便捷判定：ok() == (error == kNone)。
  bool ok() const {
    return error == VersionError::kNone;
  }
};

// 解析语义版本号。输入前提见文件头注释；严格接受恰好三段、每段纯 ASCII
// 数字且不溢出 uint32 的输入（如 "0.1.0"）。失败路径见 VersionError 说明。
ParseVersionResult parse_version(std::string_view text) noexcept;

// 当前项目版本常量。唯一事实来源是 CMakeLists.txt 的 project(VERSION ...)，
// 手工改动时必须同时修改 CMake 与这里，一致性由 version_unit 测试保证。
#define NEXWEAVE_VERSION_MAJOR 0
#define NEXWEAVE_VERSION_MINOR 1
#define NEXWEAVE_VERSION_PATCH 0
#define NEXWEAVE_VERSION_STRING "0.1.0"

}  // namespace nexweave::domain

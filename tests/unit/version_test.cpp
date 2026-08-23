// version 领域值的单元测试。
//
// 测试意图：
// - 成功路径：合法 “主.次.修订” 字符串解析出正确数值，含全零与最大值边界；
// - 失败路径：空串、缺失字段、多余字段、非数字、负数与数值溢出必须返回
//   明确的错误码，而不是抛出异常或返回”看起来成功”的默认值；
// - 幂等性：同一输入重复解析结果必须一致（纯函数承诺）；
// - 一致性不变量：头文件声明的 NEXWEAVE_VERSION_STRING 必须与 CMake 配置的
//   @PROJECT_VERSION@ 一致，且能被本模块正确解析——防止 CMake 与头文件
//   版本漂移（发布边界问题，测试失败即视为版本漂移）。
//
// 断言使用极简本地工具（无第三方依赖）：失败打印所在行并最终以非零码退出，
// 让 CTest 能直接判定失败并给出可读诊断。

#include "version.hpp"
// 构建时生成的版本头：内容来自 CMakeLists 的 project(VERSION ...)，
// 用于校验头文件常量与 CMake 配置一致。
#include "nexweave_version_from_cmake.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

// 记录一次失败断言。宏保留调用位置的行号，便于失败时定位。
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

using nexweave::domain::ParseVersionResult;
using nexweave::domain::Version;
using nexweave::domain::VersionError;
using nexweave::domain::parse_version;

// 成功路径：常规版本号。
void TestValidVersion() {
  const ParseVersionResult r = parse_version("0.1.0");
  CHECK(r.ok());
  CHECK(r.version.major == 0);
  CHECK(r.version.minor == 1);
  CHECK(r.version.patch == 0);
}

// 成功路径 + 边界值：全零与 uint32 上限都必须是合法版本号。
void TestBoundaryValues() {
  const ParseVersionResult zero = parse_version("0.0.0");
  CHECK(zero.ok());
  CHECK(zero.version.major == 0 && zero.version.minor == 0 && zero.version.patch == 0);

  const ParseVersionResult max = parse_version("4294967295.4294967295.4294967295");
  CHECK(max.ok());
  CHECK(max.version.major == 4294967295u && max.version.minor == 4294967295u &&
        max.version.patch == 4294967295u);
}

// 失败路径：空输入必须明确报告 kEmpty，而不是静默成功。
void TestEmptyInput() {
  const ParseVersionResult r = parse_version("");
  CHECK(!r.ok());
  CHECK(r.error == VersionError::kEmpty);
}

// 失败路径：缺失字段（"1.2"）与多余字段（"1.2.3.4"）必须分别得到明确错误码。
void TestWrongPartCount() {
  const ParseVersionResult missing = parse_version("1.2");
  CHECK(!missing.ok());
  CHECK(missing.error == VersionError::kMalformed);

  const ParseVersionResult extra = parse_version("1.2.3.4");
  CHECK(!extra.ok());
  CHECK(extra.error == VersionError::kTooManyParts);
}

// 失败路径：非法输入——非数字、负数、数值溢出都不得被接受。
void TestMalformedInput() {
  const ParseVersionResult alpha = parse_version("a.b.c");
  CHECK(!alpha.ok());
  CHECK(alpha.error == VersionError::kMalformed);

  const ParseVersionResult negative = parse_version("-1.2.3");
  CHECK(!negative.ok());
  CHECK(negative.error == VersionError::kMalformed);

  const ParseVersionResult overflow = parse_version("4294967296.0.0");
  CHECK(!overflow.ok());
  CHECK(overflow.error == VersionError::kMalformed);
}

// 回绕回归：超过 19 位的数字串在累加时会溢出 uint64 并回绕成小值
// （2^64 回绕为 0、2^64+1 回绕为 1），必须按溢出拒绝而非静默截断成
// 小版本号——保护"溢出直接失败"不变量，防止旧守卫被回绕绕过。
void TestOverflowWrapAround() {
  const ParseVersionResult wrap_zero = parse_version("18446744073709551616.0.0");
  CHECK(!wrap_zero.ok());
  CHECK(wrap_zero.error == VersionError::kMalformed);

  const ParseVersionResult wrap_one = parse_version("18446744073709551617.0.0");
  CHECK(!wrap_one.ok());
  CHECK(wrap_one.error == VersionError::kMalformed);
}

// 重复调用幂等：同一输入两次解析结果必须完全一致。
void TestIdempotentRepeatCall() {
  const ParseVersionResult first = parse_version("1.2.3");
  const ParseVersionResult second = parse_version("1.2.3");
  CHECK(first.ok() && second.ok());
  CHECK(first.version.major == second.version.major);
  CHECK(first.version.minor == second.version.minor);
  CHECK(first.version.patch == second.version.patch);
  CHECK(first.error == second.error);
}

// 一致性不变量：CMake 配置的版本号（构建时生成）与头文件常量必须一致，
// 且头文件声明的字符串必须能被本模块解析为相同的数值。
void TestCmakeVersionConsistency() {
  const ParseVersionResult r = parse_version(NEXWEAVE_VERSION_STRING);
  CHECK(r.ok());
  CHECK(r.version.major == NEXWEAVE_VERSION_MAJOR);
  CHECK(r.version.minor == NEXWEAVE_VERSION_MINOR);
  CHECK(r.version.patch == NEXWEAVE_VERSION_PATCH);

  // NEXWEAVE_VERSION_STRING_FROM_CMAKE 由 configure_file 从 CMakeLists 的
  // project(VERSION ...) 生成；与头文件常量不一致说明版本漂移。
  const std::string from_cmake = NEXWEAVE_VERSION_STRING_FROM_CMAKE;
  CHECK(from_cmake == NEXWEAVE_VERSION_STRING);
}

}  // namespace

int main() {
  TestValidVersion();
  TestBoundaryValues();
  TestEmptyInput();
  TestWrongPartCount();
  TestMalformedInput();
  TestOverflowWrapAround();
  TestIdempotentRepeatCall();
  TestCmakeVersionConsistency();

  if (g_failures != 0) {
    std::printf("version_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("version_test: all checks passed\n");
  return 0;
}

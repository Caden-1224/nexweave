// 所有构建配置都执行的测试断言；失败立即终止当前用例，避免继续解引用失败结果。
// 不创建线程、文件或设备。异常携带源位置并使测试进程非零退出，不受 NDEBUG 影响。
#pragma once

#include <stdexcept>
#include <string>

namespace nexweave::test {
inline void check(bool passed, const char* expression, const char* file, int line) {
  if (!passed) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression);
  }
}
}  // namespace nexweave::test

#define CHECK(expression) \
  ::nexweave::test::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

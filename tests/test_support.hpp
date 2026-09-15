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

// 带上下文的断言：当失败原因不能从表达式本身读出来时（例如“两个快照应当一致”“形态应当
// 匹配某个枚举”），把实测值一起带进诊断。没有它，失败的用例只会打印一个布尔表达式，
// 排查者必须回去重跑一遍才知道哪里不一样。
inline void check_message(bool passed, const char* expression, const std::string& detail,
                          const char* file, int line) {
  if (!passed) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression + " — " + detail);
  }
}
}  // namespace nexweave::test

#define CHECK(expression) \
  ::nexweave::test::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define CHECK_MESSAGE(expression, detail)                                               \
  ::nexweave::test::check_message(static_cast<bool>(expression), #expression, (detail),  \
                                  __FILE__, __LINE__)

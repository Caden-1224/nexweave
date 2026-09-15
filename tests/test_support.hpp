// 所有构建配置都执行的测试断言；失败立即终止当前用例，避免继续解引用失败结果。
// 不创建线程、文件或设备。异常携带源位置并使测试进程非零退出，不受 NDEBUG 影响。
// 除断言之外，这里还放几件被多个测试共用的纯文本辅助：逐行拆 JSONL、取某条指标的取值、
// 把运行清单里的日历时间归一化。它们只回答"这段文本怎么读"，不依赖任何被测对象的内部状态。
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "run_evidence.hpp"

namespace nexweave::test {

// 按行拆分 JSONL，忽略结尾换行造成的空行，使每个元素都是一条可独立解码的记录。
inline std::vector<std::string> jsonl_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t position = 0;
  while (position < text.size()) {
    const std::size_t newline = text.find('\n', position);
    if (newline == std::string::npos) {
      lines.push_back(text.substr(position));
      break;
    }
    if (newline > position) {
      lines.push_back(text.substr(position, newline - position));
    }
    position = newline + 1;
  }
  return lines;
}

// 取指标流里某个名称的首个取值；找不到返回假且不修改输出参数。用解码而不是子串搜索：
// 字段顺序由 JSON 库决定，子串断言会在字段重排时静默失效。
inline bool metric_value(const std::string& metrics_jsonl, const std::string& name,
                         double& value) {
  for (const std::string& line : jsonl_lines(metrics_jsonl)) {
    const auto metric = nexweave::observability::decode_metric(line);
    if (metric.ok() && metric.value->name == name) {
      value = metric.value->value;
      return true;
    }
  }
  return false;
}

// 指标流里某个名称是否存在（不关心取值）。
inline bool has_metric(const std::string& metrics_jsonl, const std::string& name) {
  double ignored = 0.0;
  return metric_value(metrics_jsonl, name, ignored);
}

// 去掉运行清单里的两个日历时间字段，使"两次运行的清单是否一致"可以逐字节比较。
// 确定性针对的是"同一份配置、同一个版本跑出同样的语义结果"，不针对"两次执行发生在
// 同一秒"；除这两个字段之外的全部字段（含三个哈希、命令与版本）仍要求逐字节一致。
inline std::string without_calendar_time(std::string manifest) {
  for (const char* const key : {"start_time", "end_time"}) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const std::size_t begin = manifest.find(needle);
    if (begin == std::string::npos) {
      continue;
    }
    const std::size_t value_begin = begin + needle.size();
    const std::size_t value_end = manifest.find('"', value_begin);
    if (value_end != std::string::npos) {
      manifest.replace(value_begin, value_end - value_begin, "<time>");
    }
  }
  return manifest;
}
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

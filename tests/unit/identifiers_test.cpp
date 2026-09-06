#include "identifiers.hpp"

#include <cstdio>
#include <string>

namespace {
int failures = 0;
#define CHECK(value)                     \
  do {                                   \
    if (!(value)) {                      \
      std::printf("FAIL: %s\n", #value); \
      ++failures;                        \
    }                                    \
  } while (0)

// 合法/非法/长度边界保护统一 ASCII 格式；空 session 合法，其他空标识必须拒绝。
void TestFormats() {
  CHECK(!nexweave::domain::is_valid_work_id(""));
  CHECK(nexweave::domain::is_valid_work_id("w-" + std::string(126, '1')));
  CHECK(!nexweave::domain::is_valid_work_id("w-" + std::string(127, '1')));
  CHECK(nexweave::domain::is_valid_request_id(std::string(128, 'a')));
  CHECK(!nexweave::domain::is_valid_request_id(std::string(129, 'a')));
  CHECK(!nexweave::domain::is_valid_request_id(""));
  CHECK(!nexweave::domain::is_valid_request_id("中文"));
  CHECK(nexweave::domain::is_valid_work_id("w-0"));
  CHECK(!nexweave::domain::is_valid_work_id("w-x"));
  CHECK(nexweave::domain::is_valid_request_id("request_1.v2"));
  CHECK(!nexweave::domain::is_valid_request_id("request/1"));
  CHECK(nexweave::domain::is_valid_session_id(""));
  CHECK(nexweave::domain::is_valid_session_id("session-1"));
}

// 代际每次递增，旧快照失效；本值对象的上限饱和规则由接口说明，Session 负责拒绝耗尽。
void TestGeneration() {
  nexweave::domain::Generation generation;
  CHECK(generation.current() == 0);
  CHECK(generation.advance() == 1);
  CHECK(generation.accepts(1));
  CHECK(!generation.accepts(0));
  CHECK(generation.advance() == 2);
  CHECK(!generation.accepts(1));
}
}  // namespace

int main() {
  TestFormats();
  TestGeneration();
  return failures == 0 ? 0 : 1;
}

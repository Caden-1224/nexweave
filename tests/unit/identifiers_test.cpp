#include "identifiers.hpp"

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { std::printf("FAIL: %s\\n", #value); ++failures; } } while (0)

void TestFormats() {
  CHECK(nexweave::domain::is_valid_work_id("w-0"));
  CHECK(!nexweave::domain::is_valid_work_id("w-x"));
  CHECK(nexweave::domain::is_valid_request_id("request_1.v2"));
  CHECK(!nexweave::domain::is_valid_request_id("request/1"));
  CHECK(nexweave::domain::is_valid_session_id(""));
  CHECK(nexweave::domain::is_valid_session_id("session-1"));
}

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

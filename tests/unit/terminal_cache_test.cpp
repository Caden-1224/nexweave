// 终态缓存单元测试：命中、容量淘汰、过期查询和显式失败语义。
//
// 这些用例保护的不变量：
//   - live 条目与过期标记都受容量上限约束；
//   - 容量淘汰后的查询返回 kTimeout，而不是伪装成从未见过；
//   - 超过保留期限的查询返回 kTimeout，未记录过的查询返回 kAlreadyCompleted。
#include "terminal_cache.hpp"
#include "test_support.hpp"

#include <chrono>
#include <string>
#include <thread>

namespace {

using nexweave::domain::ErrorCode;
using nexweave::runtime::TerminalCache;
using nexweave::runtime::TerminalCacheConfig;

}  // namespace

int main() {
  {
    TerminalCacheConfig config;
    config.capacity = 2;
    config.retention = std::chrono::hours(1);
    TerminalCache<std::string> cache(config);
    cache.Store("a", 1, "terminal-a");
    cache.Store("b", 2, "terminal-b");

    const auto hit = cache.Query("a", 1);
    CHECK(hit.ok());
    CHECK(*hit.value == "terminal-a");
    const auto miss = cache.Query("z", 9);
    CHECK(miss.error.code == ErrorCode::kAlreadyCompleted);

    cache.Store("c", 3, "terminal-c");
    const auto expired = cache.Query("a", 1);
    CHECK(expired.error.code == ErrorCode::kTimeout);
    const auto still_live = cache.Query("b", 2);
    CHECK(still_live.ok());
    CHECK(*still_live.value == "terminal-b");

    const auto stats = cache.stats();
    CHECK(stats.capacity == 2);
    CHECK(stats.live_entries <= 2);
    CHECK(stats.expired_markers <= 2);
    CHECK(stats.peak_live_entries <= 2);
    CHECK(stats.evicted_live >= 1);
  }

  {
    TerminalCacheConfig config;
    config.capacity = 2;
    config.retention = std::chrono::milliseconds(5);
    TerminalCache<std::string> cache(config);
    cache.Store("x", 7, "terminal-x");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto expired = cache.Query("x", 7);
    CHECK(expired.error.code == ErrorCode::kTimeout);
    CHECK(cache.stats().expired_hits == 1);
  }

  return 0;
}

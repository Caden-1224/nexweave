// 有界终态缓存：在数据面队列满、连接断开或调用方暂时不取事件时，保留可查询的终态。
//
// 职责与适用范围
// --------------
// 本类只保存“已经产生且已经定局”的值，不生成事件、不重试网络、不访问 socket。它用于
// 让终态在队列表述之外仍有一个显式容量和显式保留期限的查询路径；超过保留期限的查询
// 返回 kTimeout，未命中且从未记录返回 kAlreadyCompleted。
//
// 容量与淘汰
// ----------
// live 条目和 expired 标记都受同一个容量上限约束。容量满时淘汰最旧条目；淘汰只影响
// 可查询范围，不影响已经交付给调用方的副本。expired 标记用于把“缓存过期”与“从未见过”
// 区分开；标记自身也有容量上限，超过上限后最旧的过期标记被移除，此后查询按未命中返回。
//
// 线程与所有权
// ------------
// Store/Query/stats 分别由不同线程调用时，内部互斥量保证条目和计数的一致性。缓存拥有
// 值副本；调用方拥有传入值和返回值。查询不会阻塞、不睡眠、不等待在途产生者。
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include "../domain/error.hpp"

namespace nexweave::runtime {

// 终态缓存配置。容量为 0 或保留期限非正时构造回退默认值，避免出现“零容量”或“立刻过期”
// 的不可用配置。
struct TerminalCacheConfig {
  // live 条目和 expired 标记各自最多保留的条数。总内存上界为 2×capacity 条值副本。
  std::size_t capacity = 8;
  // 从 Store 成功起算的保留期限；查询时以单调时钟判断是否过期。
  std::chrono::milliseconds retention{30000};
};

struct TerminalCacheStats {
  std::size_t capacity = 0;
  std::size_t live_entries = 0;
  std::size_t expired_markers = 0;
  std::uint64_t stored = 0;
  std::uint64_t queried = 0;
  std::uint64_t hits = 0;
  std::uint64_t expired_hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evicted_live = 0;
  std::uint64_t evicted_expired = 0;
  std::size_t peak_live_entries = 0;
};

template <typename T>
class TerminalCache final {
 public:
  explicit TerminalCache(TerminalCacheConfig config = {}) : config_(Normalize(config)) {}

  TerminalCache(const TerminalCache&) = delete;
  TerminalCache& operator=(const TerminalCache&) = delete;

  // 保存一条终态；相同 key+generation 再次 Store 时替换旧值并刷新保留起点。
  void Store(std::string key, std::uint64_t generation, T value) {
    const std::lock_guard<std::mutex> lock(mutex_);
    PurgeExpiredLocked(std::chrono::steady_clock::now());
    for (auto entry = live_.begin(); entry != live_.end(); ++entry) {
      if (entry->key == key && entry->generation == generation) {
        entry->value = std::move(value);
        entry->stored_at = std::chrono::steady_clock::now();
        ++stats_.stored;
        return;
      }
    }
    while (live_.size() >= config_.capacity) {
      MoveOldestLiveToExpiredLocked();
    }
    live_.push_back(Entry{std::move(key), generation, std::move(value),
                          std::chrono::steady_clock::now()});
    stats_.peak_live_entries = std::max(stats_.peak_live_entries, live_.size());
    ++stats_.stored;
  }

  // 查询终态。live 命中返回副本；已过期但仍有 expired 标记返回 kTimeout；从未记录或
  // 标记已被容量淘汰返回 kAlreadyCompleted。
  domain::Result<T> Query(const std::string& key, std::uint64_t generation) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.queried;
    PurgeExpiredLocked(std::chrono::steady_clock::now());
    for (const Entry& entry : live_) {
      if (entry.key == key && entry.generation == generation) {
        ++stats_.hits;
        return domain::Result<T>::success(entry.value);
      }
    }
    for (const Marker& marker : expired_) {
      if (marker.key == key && marker.generation == generation) {
        ++stats_.expired_hits;
        return domain::Result<T>::failure(domain::ErrorCode::kTimeout,
                                          "终态缓存已经过期");
      }
    }
    ++stats_.misses;
    return domain::Result<T>::failure(domain::ErrorCode::kAlreadyCompleted,
                                      "终态缓存中没有该记录");
  }

  TerminalCacheStats stats() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    TerminalCacheStats snapshot = stats_;
    snapshot.capacity = config_.capacity;
    snapshot.live_entries = live_.size();
    snapshot.expired_markers = expired_.size();
    return snapshot;
  }

 private:
  struct Entry {
    std::string key;
    std::uint64_t generation = 0;
    T value{};
    std::chrono::steady_clock::time_point stored_at{};
  };

  struct Marker {
    std::string key;
    std::uint64_t generation = 0;
  };

  static TerminalCacheConfig Normalize(TerminalCacheConfig config) {
    if (config.capacity == 0) {
      config.capacity = 8;
    }
    if (config.retention.count() <= 0) {
      config.retention = std::chrono::seconds(30);
    }
    return config;
  }

  // 前置条件：调用方已持有 mutex_。把到期 live 条目移入 expired 标记。
  void PurgeExpiredLocked(std::chrono::steady_clock::time_point now) const {
    const auto limit = now - config_.retention;
    while (!live_.empty() && live_.front().stored_at <= limit) {
      const Entry& oldest = live_.front();
      while (expired_.size() >= config_.capacity) {
        expired_.pop_front();
        ++stats_.evicted_expired;
      }
      expired_.push_back(Marker{oldest.key, oldest.generation});
      live_.pop_front();
    }
  }

  // 前置条件：调用方已持有 mutex_ 且 live_ 非空。把最旧 live 条目淘汰为过期标记。
  void MoveOldestLiveToExpiredLocked() {
    while (expired_.size() >= config_.capacity) {
      expired_.pop_front();
      ++stats_.evicted_expired;
    }
    const Entry& oldest = live_.front();
    expired_.push_back(Marker{oldest.key, oldest.generation});
    live_.pop_front();
    ++stats_.evicted_live;
  }

  const TerminalCacheConfig config_;
  mutable std::mutex mutex_;
  mutable std::deque<Entry> live_;
  mutable std::deque<Marker> expired_;
  mutable TerminalCacheStats stats_{};
};

}  // namespace nexweave::runtime

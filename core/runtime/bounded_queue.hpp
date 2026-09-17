// 有界队列：应用队列与传输缓冲共用的容量、等待、满队列和关闭语义。
//
// 职责与适用范围
// --------------
// 本文件只提供线程安全的值队列，不解析领域事件、不访问网络、设备或进程。调用方必须先
// 声明容量、等待预算和满队列策略，再把它用于音频、文本、控制响应或终态等具体数据。
// 它解决的是“所有生产者/消费者都遵守同一组背压语义”的问题，而不是替调用方决定业务
// 收到 kTimedOut 之后是重试、取消还是终止当前流。
//
// 满队列策略
// ----------
//   - kRejectedFull：新元素不进入队列，旧元素保留。连续音频使用此策略，禁止把中间帧
//     静默替换成最新帧；
//   - kTimedOut：调用方在 push_for 中给出有限等待，超时仍放不下时返回 kTimedOut，不
//     改变队列内容；
//   - kRejectedClosed：队列关闭后不再接受新元素；
//   - kDroppedOldest：显式选择的淘汰最旧策略，只在调用方明确接受“最新覆盖最旧”时使用，
//     每次淘汰计入 dropped_oldest。
// 终态和识别音频默认使用拒绝策略；若未来允许淘汰，必须由上层记录缺口并决定是否终止当前流。
//
// 等待与关闭
// ----------
// push_for/pop_for 使用时间预算；timeout <= 0 表示只做一次非阻塞尝试。close 幂等，唤醒
// 全部等待者，但保留已经入队的数据，消费者仍可 drain 到空；clear 用于取消路径，丢弃
// 未消费数据并唤醒生产者。关闭后 pop_for 在队列为空时返回 kClosed，而不是把关闭伪装成
// 超时。
//
// 资源与线程
// ----------
// 队列创建：调用方在对象构造时提供容量；队列自身拥有元素副本，不拥有元素指向的外部资源。
// 线程安全：push/try_push/push_for 可由多个生产者调用；pop/try_pop/pop_for 可由多个消费者
// 调用；所有方法和统计由同一把互斥量线性化。元素类型必须可复制或可移动构造，析构不得抛出。
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace nexweave::runtime {

// 队列推入结果。所有非 kAccepted 结果都保证队列内容未被本次调用改变，kDroppedOldest 例外：
// 它表示新元素已经入队，同时最旧元素被显式淘汰。
enum class QueuePushStatus : std::uint8_t {
  kAccepted = 0,
  kTimedOut = 1,
  kRejectedFull = 2,
  kRejectedClosed = 3,
  kDroppedOldest = 4
};

// 队列弹出结果。kTimeout 只表示预算到期，kClosed 表示队列关闭且当前没有可消费元素。
enum class QueuePopStatus : std::uint8_t {
  kItem = 0,
  kEmpty = 1,
  kTimeout = 2,
  kClosed = 3
};

// 队列配置。容量为 0 在构造时回退为 1，避免把“零容量”解释成无界。
struct BoundedQueueConfig {
  // 当前队列中允许保留的最大元素数。它必须为正；容量是硬上限，peak_size 不会超过它。
  std::size_t capacity = 1;
  // true 时满队列淘汰最旧元素；false 时拒绝新元素。连续音频禁止使用 true。
  bool drop_oldest_on_full = false;
};

// 队列只读统计快照。所有计数只增不减，close/clear 不清零；peak_size 是历史最大深度。
struct BoundedQueueStats {
  std::size_t capacity = 1;
  std::size_t current_size = 0;
  std::size_t peak_size = 0;
  std::uint64_t push_accepted = 0;
  std::uint64_t pop_items = 0;
  std::uint64_t rejected_full = 0;
  std::uint64_t rejected_closed = 0;
  std::uint64_t dropped_oldest = 0;
  std::uint64_t waits = 0;
  std::uint64_t wait_timeouts = 0;
  bool closed = false;
};

// 线程安全的有界值队列。容量与淘汰策略是构造性质，不提供运行中扩容入口。
template <typename T>
class BoundedQueue final {
 public:
  explicit BoundedQueue(BoundedQueueConfig config = {}) : config_(Normalize(config)) {}

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // 非阻塞推入：满时按构造策略拒绝或淘汰最旧元素。关闭返回 kRejectedClosed。
  QueuePushStatus try_push(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      ++stats_.rejected_closed;
      return QueuePushStatus::kRejectedClosed;
    }
    if (items_.size() < config_.capacity) {
      PushLocked(std::move(value));
      return QueuePushStatus::kAccepted;
    }
    if (config_.drop_oldest_on_full) {
      items_.pop_front();
      ++stats_.dropped_oldest;
      PushLocked(std::move(value));
      return QueuePushStatus::kDroppedOldest;
    }
    ++stats_.rejected_full;
    return QueuePushStatus::kRejectedFull;
  }

  // 有限等待推入：timeout <= 0 等价于一次非阻塞拒绝。关闭返回 kRejectedClosed。
  QueuePushStatus push_for(T value, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) {
      ++stats_.rejected_closed;
      return QueuePushStatus::kRejectedClosed;
    }
    if (items_.size() < config_.capacity) {
      PushLocked(std::move(value));
      return QueuePushStatus::kAccepted;
    }
    if (config_.drop_oldest_on_full) {
      items_.pop_front();
      ++stats_.dropped_oldest;
      PushLocked(std::move(value));
      return QueuePushStatus::kDroppedOldest;
    }
    if (timeout.count() <= 0) {
      ++stats_.rejected_full;
      return QueuePushStatus::kRejectedFull;
    }
    ++stats_.waits;
    const bool ready = not_full_.wait_for(lock, timeout, [this] {
      return closed_ || items_.size() < config_.capacity;
    });
    if (closed_) {
      ++stats_.rejected_closed;
      return QueuePushStatus::kRejectedClosed;
    }
    if (!ready) {
      ++stats_.wait_timeouts;
      ++stats_.rejected_full;
      return QueuePushStatus::kTimedOut;
    }
    PushLocked(std::move(value));
    return QueuePushStatus::kAccepted;
  }

  // 非阻塞弹出。关闭且队列为空时返回 kClosed，避免把关闭伪装成暂时为空。
  QueuePopStatus try_pop(T& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty()) {
      return closed_ ? QueuePopStatus::kClosed : QueuePopStatus::kEmpty;
    }
    PopLocked(out);
    return QueuePopStatus::kItem;
  }

  // 有限等待弹出。timeout <= 0 等价于一次非阻塞尝试。
  QueuePopStatus pop_for(T& out, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!items_.empty()) {
      PopLocked(out);
      return QueuePopStatus::kItem;
    }
    if (closed_) {
      return QueuePopStatus::kClosed;
    }
    if (timeout.count() <= 0) {
      return QueuePopStatus::kEmpty;
    }
    ++stats_.waits;
    const bool ready = not_empty_.wait_for(lock, timeout, [this] {
      return closed_ || !items_.empty();
    });
    if (!items_.empty()) {
      PopLocked(out);
      return QueuePopStatus::kItem;
    }
    if (closed_) {
      return QueuePopStatus::kClosed;
    }
    if (!ready) {
      ++stats_.wait_timeouts;
      return QueuePopStatus::kTimeout;
    }
    return QueuePopStatus::kEmpty;
  }

  // 阻塞弹出：等待到有元素或队列关闭；关闭且已排空时返回 kClosed。
  QueuePopStatus pop_blocking(T& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });
    if (items_.empty()) {
      return QueuePopStatus::kClosed;
    }
    PopLocked(out);
    return QueuePopStatus::kItem;
  }

  // 关闭队列：幂等，唤醒所有等待者，已入队元素保留供消费者继续取走。
  void close() noexcept {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  // 清空未消费元素并唤醒生产者，供取消路径使用。返回被清空的元素数。
  std::size_t clear() noexcept {
    std::size_t removed = 0;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      removed = items_.size();
      items_.clear();
    }
    not_full_.notify_all();
    return removed;
  }

  // 取走全部剩余元素；队列的关闭状态不变，生产者仍不能继续写入。
  std::vector<T> drain() {
    std::vector<T> output;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      output.reserve(items_.size());
      while (!items_.empty()) {
        output.push_back(std::move(items_.front()));
        items_.pop_front();
      }
    }
    not_full_.notify_all();
    return output;
  }

  bool empty() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return items_.empty();
  }

  bool closed() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  std::size_t size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }

  std::size_t capacity() const noexcept {
    return config_.capacity;
  }

  std::size_t peak() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stats_.peak_size;
  }

  BoundedQueueStats stats() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    BoundedQueueStats snapshot = stats_;
    snapshot.capacity = config_.capacity;
    snapshot.current_size = items_.size();
    snapshot.closed = closed_;
    return snapshot;
  }

 private:
  static BoundedQueueConfig Normalize(BoundedQueueConfig config) {
    if (config.capacity == 0) {
      config.capacity = 1;
    }
    return config;
  }

  void PushLocked(T value) {
    items_.push_back(std::move(value));
    ++stats_.push_accepted;
    stats_.peak_size = std::max(stats_.peak_size, items_.size());
    not_empty_.notify_one();
  }

  void PopLocked(T& out) {
    out = std::move(items_.front());
    items_.pop_front();
    ++stats_.pop_items;
    not_full_.notify_one();
  }

  const BoundedQueueConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<T> items_;
  BoundedQueueStats stats_{};
  bool closed_ = false;
};

}  // namespace nexweave::runtime

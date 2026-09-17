// 有界队列单元测试：统一容量、满队列、等待、关闭、清空和峰值统计。
//
// 这些用例保护的不变量：
//   - peak_size 永不超过 capacity；
//   - kRejectedFull 与 kTimedOut 不改变队列内容；
//   - kDroppedOldest 只淘汰最旧元素并计数；
//   - close 唤醒等待者且保留已入队元素；clear 丢弃未消费元素并唤醒生产者。
#include "bounded_queue.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using nexweave::runtime::BoundedQueue;
using nexweave::runtime::BoundedQueueConfig;
using nexweave::runtime::QueuePopStatus;
using nexweave::runtime::QueuePushStatus;

}  // namespace

int main() {
  {
    BoundedQueueConfig config;
    config.capacity = 2;
    BoundedQueue<int> queue(config);
    CHECK(queue.try_push(1) == QueuePushStatus::kAccepted);
    CHECK(queue.try_push(2) == QueuePushStatus::kAccepted);
    CHECK(queue.try_push(3) == QueuePushStatus::kRejectedFull);
    CHECK(queue.peak() == 2);

    int value = 0;
    CHECK(queue.try_pop(value) == QueuePopStatus::kItem);
    CHECK(value == 1);
    CHECK(queue.try_pop(value) == QueuePopStatus::kItem);
    CHECK(value == 2);
    CHECK(queue.try_pop(value) == QueuePopStatus::kEmpty);
    const auto stats = queue.stats();
    CHECK(stats.current_size == 0);
    CHECK(stats.peak_size == 2);
    CHECK(stats.push_accepted == 2);
    CHECK(stats.pop_items == 2);
    CHECK(stats.rejected_full == 1);
  }

  {
    BoundedQueueConfig config;
    config.capacity = 2;
    config.drop_oldest_on_full = true;
    BoundedQueue<int> queue(config);
    CHECK(queue.try_push(1) == QueuePushStatus::kAccepted);
    CHECK(queue.try_push(2) == QueuePushStatus::kAccepted);
    CHECK(queue.try_push(3) == QueuePushStatus::kDroppedOldest);

    int value = 0;
    CHECK(queue.try_pop(value) == QueuePopStatus::kItem);
    CHECK(value == 2);
    CHECK(queue.try_pop(value) == QueuePopStatus::kItem);
    CHECK(value == 3);
    CHECK(queue.stats().dropped_oldest == 1);
  }

  {
    BoundedQueue<int> queue(BoundedQueueConfig{1, false});
    CHECK(queue.try_push(1) == QueuePushStatus::kAccepted);
    const auto started = std::chrono::steady_clock::now();
    CHECK(queue.push_for(2, std::chrono::milliseconds(30)) ==
          QueuePushStatus::kTimedOut);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed >= std::chrono::milliseconds(20));
    CHECK(queue.stats().wait_timeouts == 1);

    int value = 0;
    CHECK(queue.pop_for(value, std::chrono::milliseconds(0)) == QueuePopStatus::kItem);
    CHECK(value == 1);
  }

  {
    BoundedQueue<int> queue(BoundedQueueConfig{1, false});
    std::atomic<bool> producer_done{false};
    std::atomic<QueuePushStatus> producer_status{QueuePushStatus::kRejectedClosed};
    std::thread producer([&queue, &producer_done, &producer_status] {
      producer_status.store(queue.push_for(7, std::chrono::milliseconds(2000)));
      producer_done.store(true);
    });
    int value = 0;
    CHECK(queue.pop_for(value, std::chrono::milliseconds(2000)) == QueuePopStatus::kItem);
    CHECK(value == 7);
    if (producer.joinable()) {
      producer.join();
    }
    CHECK(producer_done.load());
    CHECK(producer_status.load() == QueuePushStatus::kAccepted);
  }

  {
    BoundedQueue<int> queue(BoundedQueueConfig{2, false});
    CHECK(queue.try_push(1) == QueuePushStatus::kAccepted);
    CHECK(queue.try_push(2) == QueuePushStatus::kAccepted);
    queue.close();
    queue.close();

    CHECK(queue.try_push(3) == QueuePushStatus::kRejectedClosed);
    int value = 0;
    CHECK(queue.try_pop(value) == QueuePopStatus::kItem);
    CHECK(value == 1);
    CHECK(queue.try_pop(value) == QueuePopStatus::kItem);
    CHECK(value == 2);
    CHECK(queue.try_pop(value) == QueuePopStatus::kClosed);

    std::atomic<bool> pop_done{false};
    QueuePopStatus waited = QueuePopStatus::kEmpty;
    BoundedQueue<int> empty_queue(BoundedQueueConfig{1, false});
    std::thread consumer([&empty_queue, &waited, &pop_done] {
      int ignored = 0;
      waited = empty_queue.pop_blocking(ignored);
      pop_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    empty_queue.close();
    if (consumer.joinable()) {
      consumer.join();
    }
    CHECK(pop_done.load());
    CHECK(waited == QueuePopStatus::kClosed);
  }

  {
    BoundedQueue<int> queue(BoundedQueueConfig{3, false});
    CHECK(queue.try_push(1) == QueuePushStatus::kAccepted);
    CHECK(queue.try_push(2) == QueuePushStatus::kAccepted);
    CHECK(queue.clear() == 2);
    CHECK(queue.empty());
    CHECK(queue.peak() == 2);
    const auto drained = queue.drain();
    CHECK(drained.empty());
  }

  return 0;
}

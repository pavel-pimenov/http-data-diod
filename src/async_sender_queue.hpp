#ifndef ASYNC_SENDER_QUEUE_HPP
#define ASYNC_SENDER_QUEUE_HPP

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

// Bounded FIFO queue plus the synchronization shared by the asynchronous
// delivery threads (SentryClient's error sender, JaegerLogger's span sender):
// producers push() from the request-handling threads, a single consumer thread
// waits for a batch with wait_and_take() and reports it done via
// mark_processed() so flush() can observe a fully drained queue.
//
// The queue never grows past m_max_queue_size: on overflow the configured
// policy drops either the oldest item (queue drops it, PushOutcome reports the
// drop) or the incoming one (PushOutcome::m_accepted == false), so the caller
// owns its own failure counters and the loop semantics stay in the caller.
template <typename Item> class AsyncSenderQueue {
public:
  enum class OverflowPolicy : std::uint8_t { DropOldest, DropNewest };

  // Why wait_and_take() returned.
  enum class TakeResult : std::uint8_t {
    Batch,   // `out` holds at least one item
    Idle,    // idle timeout elapsed with an empty queue (only with a timeout)
    Stopped, // stop requested, no batch was taken
  };

  struct PushOutcome {
    bool m_accepted = true; // false when the incoming item was dropped
    bool m_dropped = false; // true when an overflow drop happened
    size_t m_size = 0;      // items left in the queue after the call
  };

  AsyncSenderQueue(size_t max_queue_size, OverflowPolicy policy)
      : m_max_queue_size(max_queue_size == 0 ? 1 : max_queue_size),
        m_policy(policy) {}

  AsyncSenderQueue(const AsyncSenderQueue &) = delete;
  AsyncSenderQueue &operator=(const AsyncSenderQueue &) = delete;

  PushOutcome push(Item item) {
    PushOutcome outcome;
    {
      std::scoped_lock const lock(m_mutex);
      if (m_queue.size() >= m_max_queue_size) {
        outcome.m_dropped = true;
        if (m_policy == OverflowPolicy::DropOldest) {
          m_queue.pop_front();
          if (m_pending > 0) {
            --m_pending;
          }
        } else {
          outcome.m_accepted = false;
        }
      }
      if (outcome.m_accepted) {
        m_queue.push_back(std::move(item));
        ++m_pending;
      }
      outcome.m_size = m_queue.size();
    }
    m_cv.notify_one();
    return outcome;
  }

  // Blocks until a batch is available, until the idle timeout (ms, 0 = wait
  // indefinitely) elapses on an empty queue, or until stop is requested.
  // `drain_on_stop` picks the shutdown semantics: true keeps taking items after
  // stop was requested (the consumer loop drains the queue itself), false
  // returns Stopped immediately and leaves the leftovers to drain().
  TakeResult wait_and_take(const std::stop_token &st, int idle_timeout_ms,
                           size_t max_batch, bool drain_on_stop,
                           std::vector<Item> &out) {
    out.clear();
    std::unique_lock lock(m_mutex);
    if (m_queue.empty()) {
      if (idle_timeout_ms > 0) {
        m_cv.wait_for(lock, st, std::chrono::milliseconds(idle_timeout_ms),
                      [this] { return !m_queue.empty(); });
      } else {
        m_cv.wait(lock, st, [this] { return !m_queue.empty(); });
      }
    }
    if (m_queue.empty()) {
      return st.stop_requested() ? TakeResult::Stopped : TakeResult::Idle;
    }
    if (st.stop_requested() && !drain_on_stop) {
      return TakeResult::Stopped;
    }
    const size_t to_take = std::min(m_queue.size(), max_batch);
    out.reserve(to_take);
    for (size_t i = 0; i < to_take; ++i) {
      out.push_back(std::move(m_queue.front()));
      m_queue.pop_front();
    }
    return TakeResult::Batch;
  }

  // Marks `count` previously taken items as processed and wakes flush().
  void mark_processed(size_t count) {
    {
      std::scoped_lock const lock(m_mutex);
      m_pending = m_pending >= count ? m_pending - count : 0;
    }
    m_cv.notify_all();
  }

  // Blocks until every pushed item has been taken and marked processed.
  void flush() {
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this] { return m_pending == 0; });
  }

  // Moves every remaining item into `out` (shutdown fire-and-forget path).
  void drain(std::vector<Item> &out) {
    std::scoped_lock const lock(m_mutex);
    const size_t taken = m_queue.size();
    while (!m_queue.empty()) {
      out.push_back(std::move(m_queue.front()));
      m_queue.pop_front();
    }
    m_pending = m_pending >= taken ? m_pending - taken : 0;
  }

  // Wakes a consumer blocked in wait_and_take() (used on shutdown next to
  // std::jthread::request_stop, which the stop-token overloads observe too).
  void notify_all() {
    std::scoped_lock const lock(m_mutex);
    m_cv.notify_all();
  }

  [[nodiscard]] size_t size() const {
    std::scoped_lock const lock(m_mutex);
    return m_queue.size();
  }

private:
  const size_t m_max_queue_size;
  const OverflowPolicy m_policy;
  mutable std::mutex m_mutex;
  std::condition_variable_any m_cv;
  std::deque<Item> m_queue;
  size_t m_pending = 0; // pushed items not yet marked processed
};

#endif // ASYNC_SENDER_QUEUE_HPP

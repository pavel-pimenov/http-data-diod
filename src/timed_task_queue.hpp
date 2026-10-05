#ifndef TIMED_TASK_QUEUE_HPP
#define TIMED_TASK_QUEUE_HPP

#include "httplib/httplib.h"
#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <chrono>
#include <cstddef>
#include <functional>
#include <utility>

// httplib::TaskQueue that measures how long an accepted task waited in the
// queue before a worker thread started it. The wait spans both the queue
// itself and the time enqueue() blocks waiting for a free worker slot, so it
// is exactly the "semaphore" component of a request's latency: comparing it
// with the handler's own duration separates thread-pool saturation from
// downstream (NATS) latency.
class TimedTaskQueue final : public httplib::TaskQueue {
public:
  TimedTaskQueue(prometheus::Counter &enqueued,
                 prometheus::Counter &rejected,
                 prometheus::Histogram &wait_seconds, size_t thread_count,
                 size_t max_thread_count = 0, size_t max_queued_requests = 0,
                 time_t idle_timeout_sec = CPPHTTPLIB_THREAD_POOL_IDLE_TIMEOUT)
      : m_pool(thread_count, max_thread_count, max_queued_requests,
               idle_timeout_sec),
        m_enqueued(enqueued), m_rejected(rejected), m_wait_seconds(wait_seconds) {}

  bool enqueue(std::function<void()> fn) override {
    const auto enqueued_at = std::chrono::steady_clock::now();
    auto timed_task = [this, enqueued_at, task = std::move(fn)]() mutable {
      const auto waited = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - enqueued_at)
                              .count();
      m_wait_seconds.Observe(waited);
      task();
    };

    if (!m_pool.enqueue(std::move(timed_task))) {
      m_rejected.Increment();
      return false;
    }
    m_enqueued.Increment();
    return true;
  }

  void shutdown() override { m_pool.shutdown(); }

  void on_idle() override { m_pool.on_idle(); }

private:
  httplib::ThreadPool m_pool;
  prometheus::Counter &m_enqueued;
  prometheus::Counter &m_rejected;
  prometheus::Histogram &m_wait_seconds;
};

#endif // TIMED_TASK_QUEUE_HPP
#ifndef TIMED_TASK_QUEUE_HPP
#define TIMED_TASK_QUEUE_HPP

#include "httplib/httplib.h"
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <functional>
#include <utility>

// httplib::TaskQueue that measures how long an accepted task waited in the
// queue before a worker thread started it. The wait spans both the queue
// itself and the time enqueue() blocks waiting for a free worker slot, so it
// is exactly the "semaphore" component of a request's latency: comparing it
// with the handler's own duration separates thread-pool saturation from
// downstream (NATS) latency.
//
// The queue depth is tracked here as well (the pending gauge): httplib's
// ThreadPool keeps its job list private, so the only place that sees both the
// arrival and the start of a task is this wrapper. The depth is incremented
// on acceptance and decremented when a worker picks the task up, i.e. it
// counts the tasks that are waiting, not the ones being processed.
class TimedTaskQueue final : public httplib::TaskQueue {
public:
  TimedTaskQueue(prometheus::Counter &enqueued, prometheus::Counter &rejected,
                 prometheus::Histogram &wait_seconds,
                 prometheus::Gauge &queue_size,
                 prometheus::Gauge &queue_capacity, size_t thread_count,
                 size_t max_thread_count = 0, size_t max_queued_requests = 0,
                 time_t idle_timeout_sec = CPPHTTPLIB_THREAD_POOL_IDLE_TIMEOUT)
      : m_pool(thread_count, max_thread_count, max_queued_requests,
               idle_timeout_sec),
        m_enqueued(enqueued), m_rejected(rejected), m_wait_seconds(wait_seconds),
        m_queue_size(queue_size), m_queue_capacity(queue_capacity) {
    // 0 means "no limit enforced by the pool": the gauge mirrors the limit
    // that is actually in force, so an operator can tell "unlimited" from
    // "the queue is full".
    m_queue_capacity.Set(static_cast<double>(max_queued_requests));
  }

  bool enqueue(std::function<void()> fn) override {
    const auto enqueued_at = std::chrono::steady_clock::now();
    m_queue_size.Increment();
    auto timed_task = [this, enqueued_at, task = std::move(fn)]() mutable {
      const auto waited = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - enqueued_at)
                              .count();
      m_wait_seconds.Observe(waited);
      m_queue_size.Decrement();
      task();
    };

    if (!m_pool.enqueue(std::move(timed_task))) {
      m_queue_size.Decrement();
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
  prometheus::Gauge &m_queue_size;
  prometheus::Gauge &m_queue_capacity;
};

#endif // TIMED_TASK_QUEUE_HPP

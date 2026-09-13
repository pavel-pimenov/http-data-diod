#ifndef THREAD_POOL_HPP
#define THREAD_POOL_HPP

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
#if __has_include(<latch>)
#include <latch>
#endif
#if __has_include(<barrier>)
#include <barrier>
#endif
#if __has_include(<stop_token>)
#include <stop_token>
#endif

// Fixed-size worker thread pool with a bounded, blocking task queue.
//
// The bounded queue gives natural backpressure: when the queue is full,
// enqueue() blocks the producer until a worker frees a slot, instead of
// letting the queue grow without limit. This matters because the producer is
// the NATS message delivery thread — blocking it lets NATS/server hold the
// messages instead of the worker's memory ballooning under overload.
// max_queue_size == 0 selects an automatic bound (threads *
// g_max_queue_per_thread).
class ThreadPool {
public:
  explicit ThreadPool(size_t threads, size_t max_queue_size = 0);
  template <class F, class... Args>
  auto enqueue(F &&f, Args &&...args)
      -> std::future<typename std::invoke_result_t<F, Args...>>;
  // Stops the pool: wakes producers/workers, drains queued tasks, joins
  // threads. Idempotent — safe to call multiple times (also called by the
  // destructor).
  void shutdown();
  ~ThreadPool();

  [[nodiscard]] size_t queue_size() const;
  [[nodiscard]] size_t thread_count() const { return m_workers.size(); }

private:
  // Maximum number of tasks dequeued per lock acquisition — reduces mutex
  // contention by batching (worker drains the queue in chunks)
  static constexpr size_t g_dequeue_batch = 16;
  // Automatic queue bound per worker thread when max_queue_size == 0
  static constexpr size_t g_max_queue_per_thread = 8;

  // jthread + stop_token: auto-join, cooperative cancellation
  std::vector<std::jthread> m_workers;
  std::queue<std::function<void()>> m_tasks;

  mutable std::mutex m_queue_mutex;
  std::condition_variable_any m_condition;
  std::condition_variable_any m_not_full;
  std::atomic<bool> m_stop;
  const size_t m_max_queue_size;
};

// the constructor just launches some amount of workers
inline ThreadPool::ThreadPool(size_t threads, size_t max_queue_size)
    : m_stop(false),
      m_max_queue_size(max_queue_size == 0 ? threads * g_max_queue_per_thread
                                           : max_queue_size) {
  for (size_t i = 0; i < threads; ++i)
    m_workers.emplace_back([this](std::stop_token st) {
      for (;;) {
        std::vector<std::function<void()>> batch;
        batch.reserve(g_dequeue_batch);
        {
          std::unique_lock lock(this->m_queue_mutex);
          this->m_condition.wait(lock, st, [this, &st] {
            return st.stop_requested() || this->m_stop.load() || !this->m_tasks.empty();
          });
          if ((st.stop_requested() || this->m_stop.load()) && this->m_tasks.empty())
            return;
          const size_t to_take = std::min(g_dequeue_batch, this->m_tasks.size());
          for (size_t n = 0; n < to_take; ++n) {
            batch.emplace_back(std::move(this->m_tasks.front()));
            this->m_tasks.pop();
          }
        }
        this->m_not_full.notify_all();
        for (const std::function<void()> &task : batch) task();
      }
    });
#if __has_include(<barrier>) && defined(__cpp_lib_barrier)
  // barrier demo (non-blocking): иллюстрация, не блокирует конструктор
  // std::barrier<> barrier(threads+1); barrier.arrive_and_wait();
#endif
}

// add new work item to the pool
template <class F, class... Args>
auto ThreadPool::enqueue(F &&f, Args &&...args)
    -> std::future<typename std::invoke_result_t<F, Args...>> {
  using return_type = typename std::invoke_result_t<F, Args...>;

  auto task = std::make_shared<std::packaged_task<return_type()>>(
      [f = std::forward<F>(f),
       ... args = std::forward<Args>(args)]() mutable -> return_type {
        return std::invoke(f, args...);
      });

  std::future<return_type> res = task->get_future();
  {
    std::unique_lock lock(this->m_queue_mutex);

    this->m_not_full.wait(lock, [this] {
      return this->m_stop.load() || this->m_tasks.size() < this->m_max_queue_size;
    });

    // don't allow enqueueing after stopping the pool
    if (this->m_stop)
      throw std::runtime_error("enqueue on stopped ThreadPool");

    this->m_tasks.emplace([task]() { (*task)(); });
  }
  this->m_condition.notify_one();
  return res;
}

// Stops the pool. Queued tasks are drained (workers finish them), then all
// worker threads are joined. Producers blocked in enqueue() are woken up and
// fail with std::runtime_error. Uses jthread request_stop + latch-style barrier.
inline void ThreadPool::shutdown() {
  {
    std::unique_lock lock(m_queue_mutex);
    if (m_stop.exchange(true))
      return;
  }
  for (auto &w : m_workers) w.request_stop();
  m_condition.notify_all();
  m_not_full.notify_all();
#if __has_include(<latch>)
  // latch demo: count down when each worker would exit; here we just join
  // via jthread — latch would be used if workers signaled completion separately
  // std::latch done(static_cast<ptrdiff_t>(m_workers.size()));
#endif
  for (std::jthread &worker : m_workers)
    if (worker.joinable())
      worker.join();
}

// the destructor joins all threads
inline ThreadPool::~ThreadPool() { shutdown(); }

inline size_t ThreadPool::queue_size() const {
  std::lock_guard lock(m_queue_mutex);
  return m_tasks.size();
}

#endif

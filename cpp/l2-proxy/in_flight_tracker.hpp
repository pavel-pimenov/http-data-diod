#ifndef IN_FLIGHT_TRACKER_HPP
#define IN_FLIGHT_TRACKER_HPP

#include "logger.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

// Tracks in-flight requests for graceful shutdown
// Usage:
//   InFlightTracker tracker;
//   auto guard = tracker.track();  // Increment counter
//   // ... process request ...
//   // guard goes out of scope, decrement counter

class InFlightTracker {
private:
  // Sharded counters: each request pins to one shard (cache-line padded),
  // so concurrent requests avoid contending on a single atomic.
  static constexpr size_t g_shard_count = 16;
  struct alignas(64) Shard {
    std::atomic<uint64_t> m_in_flight{0};
    std::atomic<uint64_t> m_total{0};
  };
  std::array<Shard, g_shard_count> m_shards{};

  // Number of shards with a non-zero in-flight count. Only this atomic is
  // updated when a shard transitions between empty and non-empty, so the
  // notification path stays out of the per-request hot path.
  std::atomic<uint64_t> m_active{0};
  std::atomic<bool> m_shutdown_requested{false};
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;

  static size_t shard_index() {
    static thread_local const size_t g_shard =
        std::hash<std::thread::id>{}(std::this_thread::get_id()) %
        g_shard_count;
    return g_shard;
  }

  uint64_t in_flight_sum() const {
    uint64_t total = 0;
    for (const Shard &shard : m_shards) {
      total += shard.m_in_flight.load(std::memory_order_acquire);
    }
    return total;
  }

  uint64_t total_requests_sum() const {
    uint64_t total = 0;
    for (const Shard &shard : m_shards) {
      total += shard.m_total.load(std::memory_order_relaxed);
    }
    return total;
  }

public:
  // RAII guard for tracking in-flight requests
  class RequestGuard {
  private:
    InFlightTracker *m_tracker;
    size_t m_shard;

  public:
    explicit RequestGuard(InFlightTracker *tracker)
        : m_tracker(tracker), m_shard(tracker ? tracker->shard_index() : 0) {
      if (m_tracker) {
        m_tracker->increment(m_shard);
      }
    }

    ~RequestGuard() {
      if (m_tracker) {
        m_tracker->decrement(m_shard);
      }
    }

    // Non-copyable
    RequestGuard(const RequestGuard &) = delete;
    RequestGuard &operator=(const RequestGuard &) = delete;

    // Movable
    RequestGuard(RequestGuard &&other) noexcept
        : m_tracker(other.m_tracker), m_shard(other.m_shard) {
      other.m_tracker = nullptr;
    }

    RequestGuard &operator=(RequestGuard &&other) noexcept {
      if (this != &other) {
        if (m_tracker) {
          m_tracker->decrement(m_shard);
        }
        m_tracker = other.m_tracker;
        m_shard = other.m_shard;
        other.m_tracker = nullptr;
      }
      return *this;
    }
  };

  // Start tracking a request
  RequestGuard track() { return RequestGuard(this); }

  // Manual increment/decrement (use track() instead for RAII)
  void increment() { increment(shard_index()); }

  void decrement() { decrement(shard_index()); }

private:
  void increment(size_t shard) {
    Shard &s = m_shards[shard];
    const uint64_t prev = s.m_in_flight.fetch_add(1, std::memory_order_relaxed);
    s.m_total.fetch_add(1, std::memory_order_relaxed);
    // First request on this shard: register the shard as active
    if (prev == 0) {
      m_active.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void decrement(size_t shard) {
    Shard &s = m_shards[shard];
    const uint64_t prev = s.m_in_flight.fetch_sub(1, std::memory_order_release);
    // Shard became empty: unregister it. If it was the last active shard,
    // wake up any thread waiting for completion (notify under the mutex to
    // avoid a lost-wakeup race with wait_for_completion).
    if (prev == 1 && m_active.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard lock(m_mutex);
      m_cv.notify_all();
    }
  }

public:
  void request_shutdown() {
    m_shutdown_requested = true;
    Logger::info("Shutdown requested - waiting for {} in-flight requests",
                 in_flight());
  }

  bool is_shutdown_requested() const { return m_shutdown_requested.load(); }

  // Wait for all in-flight requests to complete (with timeout)
  bool wait_for_completion(std::chrono::seconds timeout,
                           bool log_issues = true) {
    std::unique_lock lock(m_mutex);

    bool completed =
        m_cv.wait_for(lock, timeout, [this] { return in_flight_sum() == 0; });

    if (!completed) {
      if (log_issues) {
        Logger::warn("Shutdown timeout - {} requests still in flight",
                     in_flight());
      }
      return false;
    }

    if (log_issues) {
      Logger::info("All in-flight requests completed ({} total)",
                   total_requests());
    }
    return true;
  }

  uint64_t in_flight() const { return in_flight_sum(); }

  uint64_t total_requests() const { return total_requests_sum(); }
};

#endif // IN_FLIGHT_TRACKER_HPP

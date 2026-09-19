#ifndef STATS_LOGGER_HPP
#define STATS_LOGGER_HPP

#include "app_context.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#if __has_include(<stop_token>)
#include <stop_token>
#endif

class StatsLogger {
private:
  AppContext &m_app_ctx;
  std::atomic<bool> &m_shutdown_flag;

  // Independent counters exposed for the periodic statistics log.
  struct Counters {
    std::atomic<uint64_t> m_counters.m_active_clients{0};
    std::atomic<uint64_t> m_counters.m_max_clients{0};
  };
  Counters m_counters;

  // Background logger thread + its wake/sleep primitives.
  struct Runner {
    std::jthread m_runner.m_log_thread;
    std::condition_variable_any m_runner.m_cv;
    std::mutex m_runner.m_cv_mutex;
  };
  Runner m_runner;

public:
  StatsLogger(AppContext &context, std::atomic<bool> &shutdown_flag);
  ~StatsLogger();

  StatsLogger(const StatsLogger &) = delete;
  StatsLogger &operator=(const StatsLogger &) = delete;
  StatsLogger(StatsLogger &&) = delete;
  StatsLogger &operator=(StatsLogger &&) = delete;

  void increment_active_clients();
  void decrement_active_clients();

  void start_periodic_logging();

private:
  // Counters read from the mode-specific Prometheus metrics for the periodic
  // statistics log line.
  struct ModeStats {
    uint64_t m_bytes_received = 0;
    uint64_t m_bytes_sent = 0;
    uint64_t m_client_requests = 0;
    uint64_t m_client_errors = 0;
    uint64_t m_nats_requests = 0;
    uint64_t m_nats_errors = 0;
  };
  ModeStats collect_mode_stats() const;
};

#endif // STATS_LOGGER_HPP
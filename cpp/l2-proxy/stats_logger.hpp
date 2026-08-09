#ifndef STATS_LOGGER_HPP
#define STATS_LOGGER_HPP

#include "app_context.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

class StatsLogger {
private:
  AppContext &m_app_ctx;
  std::atomic<bool> &m_shutdown_flag;

  std::atomic<uint64_t> m_active_clients{0};
  std::atomic<uint64_t> m_max_clients{0};
  std::atomic<uint64_t> m_total_requests{0};
  std::chrono::steady_clock::time_point m_start_time;

  std::thread m_log_thread;

public:
  StatsLogger(AppContext &context, std::atomic<bool> &shutdown_flag);
  ~StatsLogger();

  StatsLogger(const StatsLogger &) = delete;
  StatsLogger &operator=(const StatsLogger &) = delete;
  StatsLogger(StatsLogger &&) = delete;
  StatsLogger &operator=(StatsLogger &&) = delete;

  void increment_active_clients();
  void decrement_active_clients();
  void increment_total_requests();

  uint64_t get_active_clients() const { return m_active_clients.load(); }
  uint64_t get_max_clients() const { return m_max_clients.load(); }
  uint64_t get_total_requests() const { return m_total_requests.load(); }

  void start_periodic_logging();

private:
  void log_statistics();
};

#endif // STATS_LOGGER_HPP
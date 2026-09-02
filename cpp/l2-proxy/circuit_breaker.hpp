#ifndef CIRCUIT_BREAKER_HPP
#define CIRCUIT_BREAKER_HPP

#include "logger.hpp"
#include "time_utils.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <prometheus/gauge.h>

struct CircuitBreaker {
  enum class State : std::uint8_t { CLOSED = 0, OPEN = 1, HALF_OPEN = 2 };

  prometheus::Gauge *m_gauge = nullptr;
  std::atomic<State> m_state{State::CLOSED};
  std::atomic<int> m_failure_count{0};
  std::atomic<int> m_success_count{0};
  std::atomic<uint64_t> m_last_failure_time_us{0};

  static constexpr int g_failure_threshold = 5;
  static constexpr uint64_t g_open_timeout_us = 10'000'000; // 10 seconds
  static constexpr int g_half_open_success_threshold = 2;

  void set_gauge(prometheus::Gauge *gauge);
  [[nodiscard]] bool allow_request();
  void record_success();
  void record_failure();
  std::string state_name() const;

private:
  void update_gauge();
};

#endif // CIRCUIT_BREAKER_HPP

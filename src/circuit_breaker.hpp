#ifndef CIRCUIT_BREAKER_HPP
#define CIRCUIT_BREAKER_HPP

#include "logger.hpp"
#include "time_utils.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>

struct CircuitBreaker {
  enum class State : std::uint8_t { CLOSED = 0, OPEN = 1, HALF_OPEN = 2 };

  std::optional<std::reference_wrapper<prometheus::Gauge>> m_gauge;
  // Optional observability hooks: transitions labelled by target state
  // ("open"/"half_open"/"closed") and requests fast-failed while OPEN. Unset in
  // unit tests that only exercise the state machine.
  std::optional<std::reference_wrapper<prometheus::Family<prometheus::Counter>>>
      m_transitions;
  std::optional<std::reference_wrapper<prometheus::Counter>> m_rejected;
  std::atomic<State> m_state{State::CLOSED};
  struct Counters {
    std::atomic<int> m_failure_count{0};
    std::atomic<int> m_success_count{0};
    std::atomic<uint64_t> m_last_failure_time_us{0};
  } m_counters;

  static constexpr int g_failure_threshold = 5;
  static constexpr uint64_t g_open_timeout_us = 10'000'000; // 10 seconds
  static constexpr int g_half_open_success_threshold = 2;

  void set_gauge(prometheus::Gauge &gauge);
  void set_counters(prometheus::Family<prometheus::Counter> &transitions,
                    prometheus::Counter &rejected);
  [[nodiscard]] bool allow_request();
  void record_success();
  void record_failure();
  [[nodiscard]] std::string state_name() const;

private:
  void update_gauge();
  void transition_to_open();
  void record_transition(const char *to);
};

#endif // CIRCUIT_BREAKER_HPP

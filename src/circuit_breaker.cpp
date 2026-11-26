#include "circuit_breaker.hpp"
#include "logger.hpp"
#include "time_utils.hpp"
#include <cstdint>
#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>

void CircuitBreaker::set_gauge(prometheus::Gauge &gauge) {
  m_gauge = gauge;
  update_gauge();
}

void CircuitBreaker::set_counters(
    prometheus::Family<prometheus::Counter> &transitions,
    prometheus::Counter &rejected) {
  m_transitions = transitions;
  m_rejected = rejected;
}

void CircuitBreaker::record_transition(const char *to) {
  if (m_transitions.has_value()) {
    m_transitions->get().Add({{"to", to}}).Increment();
  }
}

void CircuitBreaker::update_gauge() {
  if (m_gauge.has_value()) {
    m_gauge->get().Set(static_cast<double>(m_state.load()));
  }
}

bool CircuitBreaker::allow_request() {
  const auto current_state = m_state.load();
  if (current_state == State::CLOSED) {
    return true;
  }
  if (current_state == State::HALF_OPEN) {
    return true;
  }
  // OPEN state: check if timeout has elapsed (monotonic clock so wall-clock
  // jumps cannot flip the breaker)
  const uint64_t now_us = TimeUtils::steady_us();
  const uint64_t elapsed = now_us - m_counters.m_last_failure_time_us.load();
  if (elapsed >= g_open_timeout_us) {
    // Atomically OPEN -> HALF_OPEN so only one thread wins the transition
    // (others re-read the state instead of racing on a load->store pair).
    State expected = State::OPEN;
    if (m_state.compare_exchange_strong(expected, State::HALF_OPEN)) {
      Logger::info("Circuit breaker: OPEN -> HALF_OPEN (timeout elapsed)");
      record_transition("half_open");
      m_counters.m_success_count.store(0);
      update_gauge();
      return true;
    }
    // Lost the race: another thread already left OPEN; a request is allowed
    // unless it is still OPEN.
    return m_state.load() != State::OPEN;
  }
  if (m_rejected.has_value()) {
    m_rejected->get().Increment();
  }
  return false;
}

void CircuitBreaker::record_success() {
  const auto current_state = m_state.load();
  if (current_state == State::HALF_OPEN) {
    const int count = m_counters.m_success_count.fetch_add(1) + 1;
    if (count >= g_half_open_success_threshold) {
      // Atomically HALF_OPEN -> CLOSED: only the thread that reaches the
      // threshold closes the breaker, others observe the new state.
      State expected = State::HALF_OPEN;
      if (m_state.compare_exchange_strong(expected, State::CLOSED)) {
        Logger::info("Circuit breaker: HALF_OPEN -> CLOSED (successes={})",
                     count);
        record_transition("closed");
        m_counters.m_failure_count.store(0);
        m_counters.m_success_count.store(0);
        update_gauge();
      }
    }
  } else if (current_state == State::CLOSED) {
    m_counters.m_failure_count.store(0);
  }
}

void CircuitBreaker::record_failure() {
  const auto current_state = m_state.load();
  if (current_state == State::HALF_OPEN) {
    // Atomically HALF_OPEN -> OPEN; a concurrent success that closes the
    // breaker first wins and this failure is dropped.
    State expected = State::HALF_OPEN;
    if (m_state.compare_exchange_strong(expected, State::OPEN)) {
      Logger::warn("Circuit breaker: HALF_OPEN -> OPEN (test request failed)");
      record_transition("open");
      m_counters.m_last_failure_time_us.store(TimeUtils::steady_us());
      m_counters.m_success_count.store(0);
      update_gauge();
    }
  } else if (current_state == State::CLOSED) {
    const int count = m_counters.m_failure_count.fetch_add(1) + 1;
    if (count >= g_failure_threshold) {
      // Atomically CLOSED -> OPEN: only one thread trips the breaker.
      State expected = State::CLOSED;
      if (m_state.compare_exchange_strong(expected, State::OPEN)) {
        Logger::warn("Circuit breaker: CLOSED -> OPEN (failures={})", count);
        record_transition("open");
        m_counters.m_last_failure_time_us.store(TimeUtils::steady_us());
        m_counters.m_success_count.store(0);
        update_gauge();
      }
    }
  }
  // OPEN state: keep the original failure timestamp so late (in-flight)
  // requests that fail after the trip do not extend the open window.
}

std::string CircuitBreaker::state_name() const {
  switch (m_state.load()) {
  case State::CLOSED:
    return "CLOSED";
  case State::OPEN:
    return "OPEN";
  case State::HALF_OPEN:
    return "HALF_OPEN";
  }
  return "UNKNOWN";
}

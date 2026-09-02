#include "circuit_breaker.hpp"
#include "logger.hpp"

void CircuitBreaker::set_gauge(prometheus::Gauge *gauge) {
  m_gauge = gauge;
  update_gauge();
}

void CircuitBreaker::update_gauge() {
  if (m_gauge != nullptr) {
    m_gauge->Set(static_cast<double>(m_state.load()));
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
  // OPEN state: check if timeout has elapsed
  const uint64_t now_us = static_cast<uint64_t>(TimeUtils::epoch_us());
  const uint64_t elapsed = now_us - m_last_failure_time_us.load();
  if (elapsed >= g_open_timeout_us) {
    Logger::info("Circuit breaker: OPEN -> HALF_OPEN (timeout elapsed)");
    m_state.store(State::HALF_OPEN);
    m_success_count.store(0);
    update_gauge();
    return true;
  }
  return false;
}

void CircuitBreaker::record_success() {
  const auto current_state = m_state.load();
  if (current_state == State::HALF_OPEN) {
    const int count = m_success_count.fetch_add(1) + 1;
    if (count >= g_half_open_success_threshold) {
      Logger::info("Circuit breaker: HALF_OPEN -> CLOSED (successes={})",
                   count);
      m_state.store(State::CLOSED);
      m_failure_count.store(0);
      m_success_count.store(0);
      update_gauge();
    }
  } else if (current_state == State::CLOSED) {
    m_failure_count.store(0);
  }
}

void CircuitBreaker::record_failure() {
  m_last_failure_time_us.store(static_cast<uint64_t>(TimeUtils::epoch_us()));
  const auto current_state = m_state.load();
  if (current_state == State::HALF_OPEN) {
    Logger::warn("Circuit breaker: HALF_OPEN -> OPEN (test request failed)");
    m_state.store(State::OPEN);
    m_success_count.store(0);
    update_gauge();
  } else if (current_state == State::CLOSED) {
    const int count = m_failure_count.fetch_add(1) + 1;
    if (count >= g_failure_threshold) {
      Logger::warn("Circuit breaker: CLOSED -> OPEN (failures={})", count);
      m_state.store(State::OPEN);
      update_gauge();
    }
  }
  // OPEN state: already tracking via last_failure_time_us
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

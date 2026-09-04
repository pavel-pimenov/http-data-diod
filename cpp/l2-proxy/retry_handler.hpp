#ifndef RETRY_HANDLER_HPP
#define RETRY_HANDLER_HPP

#include <algorithm>

class RetryHandler {
public:
  explicit RetryHandler(int initial_delay_ms = 100, int max_delay_ms = 2000)
      : m_initial_delay_ms(initial_delay_ms), m_max_delay_ms(max_delay_ms),
        m_current_delay_ms(initial_delay_ms), m_consecutive_failures(0) {}

  void record_failure() {
    m_consecutive_failures++;
    m_current_delay_ms = std::min(m_current_delay_ms * 2, m_max_delay_ms);
  }

  void record_success() {
    m_consecutive_failures = 0;
    m_current_delay_ms = m_initial_delay_ms;
  }

  int get_consecutive_failures() const { return m_consecutive_failures; }
  int get_current_delay_ms() const { return m_current_delay_ms; }

private:
  int m_initial_delay_ms;
  int m_max_delay_ms;
  int m_current_delay_ms;
  int m_consecutive_failures = 0;
};

#endif // RETRY_HANDLER_HPP

#ifndef RETRY_HANDLER_HPP
#define RETRY_HANDLER_HPP

#include <algorithm>

class RetryHandler {
public:
  explicit RetryHandler(int initial_delay_ms = 100, int max_delay_ms = 2000)
      : m_config{.m_initial_delay_ms = initial_delay_ms,
                 .m_max_delay_ms = max_delay_ms} {
    m_state.m_current_delay_ms = m_config.m_initial_delay_ms;
  }

  void record_failure() {
    m_state.m_consecutive_failures++;
    m_state.m_current_delay_ms =
        std::min(m_state.m_current_delay_ms * 2, m_config.m_max_delay_ms);
  }

  void record_success() {
    m_state.m_consecutive_failures = 0;
    m_state.m_current_delay_ms = m_config.m_initial_delay_ms;
  }

  int get_consecutive_failures() const { return m_state.m_consecutive_failures; }
  int get_current_delay_ms() const { return m_state.m_current_delay_ms; }

private:
  struct Config {
    int m_initial_delay_ms;
    int m_max_delay_ms;
  } m_config;

  struct State {
    int m_current_delay_ms;
    int m_consecutive_failures = 0;
  } m_state;
};

#endif // RETRY_HANDLER_HPP

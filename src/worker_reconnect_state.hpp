#ifndef WORKER_RECONNECT_STATE_HPP
#define WORKER_RECONNECT_STATE_HPP

struct WorkerReconnectState {
  bool m_was_connected = false;
  bool m_subscription_active = false;
  bool m_db_subscription_active = false;

  bool observe_connection(bool is_connected) {
    const bool restored = is_connected && !m_was_connected;
    if (restored) {
      m_subscription_active = false;
      m_db_subscription_active = false;
    }
    m_was_connected = is_connected;
    return restored;
  }

  void set_subscription_active(bool active) { m_subscription_active = active; }

  void set_db_subscription_active(bool active) {
    m_db_subscription_active = active;
  }

  void clear_on_disconnect(bool client_available, bool is_connected) {
    if (client_available && !is_connected) {
      m_subscription_active = false;
      m_db_subscription_active = false;
    }
  }

  [[nodiscard]] bool backoff_failed(bool client_available,
                                    bool is_connected) const {
    return !m_subscription_active || !client_available || !is_connected;
  }
};

inline bool db_init_retry_due(int &retry_count, bool handler_available,
                              bool all_configured, int every_passes) {
  if (!handler_available || all_configured || every_passes <= 0) {
    return false;
  }
  return ++retry_count % every_passes == 0;
}

#endif // WORKER_RECONNECT_STATE_HPP

#ifndef READINESS_HPP
#define READINESS_HPP

#include <array>
#include <format>
#include <string>

// NATS readiness contract shared by the proxy handler and the AppContext
// metrics registration. The reasons are the single source of truth: the
// AppContext pre-creates a series per reason, and the handlers use the same
// constants so a metric label can never drift from the body text.
inline constexpr const char *kNotReadyNatsUnavailable = "nats_unavailable";
inline constexpr const char *kNotReadyPingFailed = "ping_failed";
inline constexpr const char *kNotReadyHealthCheckFailed = "health_check_failed";
inline constexpr std::array<const char *, 3> g_not_ready_reasons = {
    kNotReadyNatsUnavailable, kNotReadyPingFailed, kNotReadyHealthCheckFailed};

namespace readiness {

// Snapshot of the NATS checks performed by /health/ready. Kept free of
// NatsClient/AppContext so the branch matrix (client missing, disconnected,
// allow-connect ping success/failure, exception) is unit-testable without a
// live broker.
struct NatsProbe {
  bool m_has_client = false;
  bool m_allow_connect = false;
  bool m_connected = false;
  bool m_ping_ok = false;
};

struct Result {
  bool m_ready = false;
  const char *m_reason = nullptr;
  std::string m_error;

  explicit operator bool() const { return m_ready; }
};

inline Result nats_unavailable() {
  return {.m_ready = false,
          .m_reason = kNotReadyNatsUnavailable,
          .m_error = "NATS connection not available"};
}

inline Result evaluate_nats(const NatsProbe &probe) {
  if (!probe.m_has_client || !probe.m_connected) {
    return nats_unavailable();
  }
  // The legacy opt-in path pings the broker (and may reconnect); the default
  // path trusts the connection state only and must never block the probe.
  if (probe.m_allow_connect && !probe.m_ping_ok) {
    return {.m_ready = false,
            .m_reason = kNotReadyPingFailed,
            .m_error = "NATS ping failed"};
  }
  return {.m_ready = true, .m_reason = nullptr, .m_error = {}};
}

inline Result nats_check_failed(const std::string &what) {
  return {.m_ready = false,
          .m_reason = kNotReadyHealthCheckFailed,
          .m_error = std::format("NATS health check failed: {}", what)};
}

} // namespace readiness

#endif // READINESS_HPP

#ifndef SENTRY_CLIENT_HPP
#define SENTRY_CLIENT_HPP

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <atomic>
#include <mutex>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sentry {

enum class EventLevel : std::uint8_t { Debug, Info, Warning, Error, Fatal };

// Parsed parts of a Sentry DSN
// ("https://PUBLIC[:SECRET]@HOST[:PORT][/PATH]/PROJECT"). Kept
// dependency-free so the parser is unit-testable without a network.
struct DsnData {
  std::string m_scheme;      // "https" | "http"
  std::string m_host;        // host without port
  int m_port = 0;            // 443 for https / 80 for http unless specified
  std::string m_path_prefix; // "" for sentry.io, "/subpath" for self-hosted
  std::string m_public_key;
  std::string m_secret_key;
  std::string m_project_id;
};

// Minimal Sentry event payload. Only the populated fields appear in the wire
// JSON, keeping each envelope small.
struct SentryEvent {
  std::string m_message;
  EventLevel m_level{EventLevel::Error};
  std::string m_transaction;
  std::string m_request_id;
  nlohmann::json m_tags = nlohmann::json::object();
  std::vector<std::string> m_fingerprint;
  nlohmann::json m_extra = nlohmann::json::object();
};

// Pure helpers (no network / no registry) so they are unit-testable directly.
std::optional<DsnData> parse_dsn(std::string_view dsn);
nlohmann::json build_event_json(const SentryEvent &event,
                                const std::string &service_name,
                                const std::string &environment,
                                const std::string &release);
std::string build_envelope(const SentryEvent &event, const DsnData &dsn,
                           const std::string &service_name,
                           const std::string &environment,
                           const std::string &release);
std::string level_to_string(EventLevel level);

} // namespace sentry

// Asynchronous, non-blocking Sentry event sender. The hot path only enqueues;
// a background thread assembles and posts the envelope. Errors are swallowed
// and counted via Prometheus, never propagated to request processing.
class SentryClient {
public:
  using TransportFn = std::function<bool(const std::string &envelope)>;

  SentryClient(std::string dsn, prometheus::Counter &events_sent,
               prometheus::Counter &events_failed, prometheus::Gauge &queue_size,
               std::string service_name = "unknown",
               std::string environment = "", std::string release = "",
               int timeout_ms = 3000, size_t max_queue_size = 256,
               const TransportFn &transport = nullptr);
  ~SentryClient();
  SentryClient(const SentryClient &) = delete;
  SentryClient &operator=(const SentryClient &) = delete;

  bool enabled() const;

  // Enqueues the event for asynchronous delivery. No-op when disabled.
  void capture(const sentry::SentryEvent &event);
  void capture_message(const std::string &message,
                       const std::string &request_id = "",
                       const std::vector<std::string> &fingerprint = {});

  // Blocks until the queue is drained. Used by shutdown and tests.
  void flush();

private:
  void sender_loop(std::stop_token st);
  void process_event(const sentry::SentryEvent &event);
  bool send_envelope(const std::string &envelope);

  std::string m_dsn;
  std::optional<sentry::DsnData> m_dsn_data;
  std::string m_service_name;
  std::string m_environment;
  std::string m_release;
  prometheus::Counter &m_events_sent;
  prometheus::Counter &m_events_failed;
  prometheus::Gauge &m_queue_size;
  int m_timeout_ms;
  size_t m_max_queue_size;
  TransportFn m_transport;

  std::mutex m_mutex;
  std::condition_variable_any m_cv;
  std::deque<sentry::SentryEvent> m_queue;
  std::jthread m_sender_thread;
  std::atomic<size_t> m_pending{0};
};

#endif // SENTRY_CLIENT_HPP
#ifndef TRACE_LOGGER_HPP
#define TRACE_LOGGER_HPP

#include "nlohmann/json.hpp"
#include "sentry_client.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <stop_token>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>

// Forward declarations to avoid transitive httplib include
class HttpClientPool;

// Tracing constants
inline constexpr size_t g_tracing_max_queue_size =
    10000; // Maximum spans in queue before dropping
inline constexpr int g_tracing_default_batch_size =
    50; // Default batch size for sending
inline constexpr int g_tracing_send_interval_ms =
    1000;                                       // Default flush interval in ms
inline constexpr int g_tracing_max_retries = 3; // Maximum retry attempts
inline constexpr int g_tracing_retry_base_delay_ms =
    100; // Base delay for exponential backoff
inline constexpr int g_tracing_send_timeout_ms =
    5000; // HTTP send timeout in ms
// Tuning of the tracing outage circuit-breaker (предохранитель). After
// m_failure_threshold consecutive delivery failures the target (Jaeger or the
// optional Sentry/GlitchTip performance sink) is considered out of order and
// delivery is shed for an exponential cooldown window (m_cooldown_base_ms,
// doubled on every consecutive opening, clamped to m_cooldown_max_ms) instead
// of bombarding the dead target with one POST per flush interval forever.
struct TracingBreakerSettings {
  int m_failure_threshold = 3;    // Consecutive failures before opening
  int m_cooldown_base_ms = 1000;  // First cooldown window after opening, ms
  int m_cooldown_max_ms = 30000;  // Upper clamp of the cooldown window, ms
};

class JaegerLogger {
private:
  std::string m_jaeger_url;
  prometheus::Counter &m_tracing_spans_sent_counter;
  prometheus::Counter &m_tracing_spans_failed_counter;
  prometheus::Gauge &m_tracing_queue_size_gauge;
  prometheus::Gauge &m_tracing_last_send_duration_gauge;

  prometheus::Histogram &m_tracing_send_latency_histogram;
  prometheus::Histogram &m_tracing_queue_time_histogram;

  std::unique_ptr<HttpClientPool> m_http_client_pool;
  // Dedicated pool for the Sentry/GlitchTip target: HttpClient caches its
  // connection for the first host it sees, so reusing the Jaeger pool would
  // send Sentry envelopes to the Jaeger host.
  std::unique_ptr<HttpClientPool> m_sentry_client_pool;

  struct SpanData {
    std::string m_trace_id, m_span_id, m_parent_id, m_name, m_service_name;
    uint64_t m_start_us, m_end_us;
    uint64_t m_enqueue_time_us; // For queue time measurement
    nlohmann::json m_attributes;
  };

  std::deque<SpanData> m_span_queue;
  std::mutex m_queue_mutex;
  std::condition_variable_any m_queue_cv;
  std::jthread m_sender_thread;

  // Optional Sentry/GlitchTip performance target: when set, every delivered
  // span batch is additionally POSTed to the DSN's envelope endpoint as a
  // Sentry "transaction" so the Performance → Transaction Groups view of the
  // self-hosted Sentry-compatible server (glitchtip) is populated.
  std::optional<sentry::DsnData> m_sentry_dsn;
  std::string m_sentry_service;
  std::string m_sentry_environment;
  std::string m_sentry_release;
  // Separate trace sampling for the Sentry/GlitchTip target: independent of
  // the Jaeger sample rate so the two sinks can be tuned differently.
  double m_sentry_sample_rate{1.0};
  prometheus::Counter *m_sentry_spans_sent;
  prometheus::Counter *m_sentry_spans_failed;

  // Configuration
  size_t m_batch_size;
  int m_flush_interval_ms;
  double m_sample_rate; // 0.0-1.0, 1.0 = 100% sampling

  // Retry state
  std::atomic<int> m_consecutive_failures{0};

  // Exponential circuit breaker state shared by the Jaeger and
  // Sentry/GlitchTip sinks. Both are "one multi-item POST per batch" style
  // targets: Jaeger with a retry loop of up to 3 POSTs per batch,
  // Sentry/GlitchTip with a single multi-item envelope POST per batch and no
  // retry loop at all. During a long target outage each sink would otherwise
  // keep firing its POST per batch forever. After the consecutive-failure
  // threshold (TracingBreakerSettings::m_failure_threshold) the breaker opens:
  // delivery of the batch is shed for the exponential cooldown window (capped
  // by m_cooldown_max_ms) instead of bombarding the dead target. Closing the
  // breaker resets the consecutive-failure counter.
  struct ExponentialBreaker {
    std::atomic<int> consecutive_failures{0};
    std::atomic<uint64_t> cooldown_until_steady_ms{0};
  };
  ExponentialBreaker m_sentry_breaker;
  ExponentialBreaker m_jaeger_breaker;

  // Per-sink breaker tuning (thresholds/cooldowns).
  TracingBreakerSettings m_breaker_settings;

private:
  bool send_batch(const std::vector<SpanData> &batch);
  // Sends one batch with retries and records timing/queue/counter metrics.
  void send_batch_with_retry(const std::vector<SpanData> &batch,
                             const std::stop_token &st);
  // Delivers a batch to the optional Sentry/GlitchTip performance target
  // (one multi-item envelope per batch). Fire-and-forget, never blocks the
  // request path; per-span outcome lands in m_sentry_spans_sent/_failed.
  void deliver_sentry_transactions(const std::vector<SpanData> &batch);

  // Fast random hex generation (thread-local, no re-initialization)
  static std::string random_hex_fast(size_t len);

public:
  // Build a single Zipkin v2 span JSON object (Jaeger /api/v2/spans schema).
  // Pure helper (no member state) so it is unit-testable without a registry.
  static nlohmann::json build_span_json(const std::string &trace_id,
                                        const std::string &span_id,
                                        const std::string &parent_id,
                                        const std::string &name,
                                        uint64_t start_us, uint64_t end_us,
                                        const std::string &service_name,
                                        const nlohmann::json &attributes) {
    nlohmann::json tags = nlohmann::json::object();
    for (const auto &el : attributes.items()) {
      if (el.value().is_string()) {
        tags[el.key()] = el.value();
      } else {
        tags[el.key()] = el.value().dump();
      }
    }

    nlohmann::json span = nlohmann::json::object();
    span["id"] = span_id;
    span["traceId"] = trace_id;
    span["name"] = name;
    span["timestamp"] = start_us;
    span["duration"] = end_us - start_us;
    span["localEndpoint"] = {{"serviceName", service_name}};
    span["tags"] = tags;

    if (!parent_id.empty()) {
      span["parentId"] = parent_id;
    }

    return span;
  }

  // Sentry trace/timing helpers for the performance delivery. Pure static
  // functions (no registry / no network) so they are unit-testable directly.

  // Maps a span operation tag to a Sentry-context "op" value.
  static std::string sentry_span_op(const std::string &name);

  // Maps an HTTP status (found in the span attributes) to a Sentry trace
  // status: <500 → "ok", errors → "internal_error".
  static std::string sentry_transaction_status(int status_code);

  // Builds the Sentry "transaction" event JSON body for a single span.
  // `product` (the runtime MODE, e.g. "proxy"/"worker"/"l2-server") prefixes
  // the transaction name and lands in a "mode" tag so that Transaction Groups
  // separate by product instead of merging across the services.
  static nlohmann::json
  build_sentry_transaction_json(const std::string &trace_id,
                                const std::string &span_id,
                                const std::string &parent_id,
                                const std::string &name,
                                const std::string &service_name,
                                uint64_t start_us, uint64_t end_us,
                                const std::string &environment,
                                const nlohmann::json &attributes,
                                const std::string &release = "",
                                const std::string &product = "");

  // Envelope endpoint URL for a DSN ("/{path_prefix}/api/{project}/envelope/").
  static std::string sentry_envelope_url(const sentry::DsnData &dsn);

  // X-Sentry-Auth header value for a DSN.
  static std::string sentry_auth_header(const sentry::DsnData &dsn);

  // Full Sentry envelope (header + transaction item + JSON body) for one span.
  static std::string
  build_sentry_transaction_envelope(const std::string &trace_id,
                                    const std::string &span_id,
                                    const std::string &parent_id,
                                    const std::string &name,
                                    const std::string &service_name,
                                    uint64_t start_us, uint64_t end_us,
                                    const sentry::DsnData &dsn,
                                    const std::string &environment,
                                    const nlohmann::json &attributes,
                                    const std::string &release = "",
                                    const std::string &product = "");

  // Full Sentry envelope for an already-built transaction event JSON.
  static std::string
  build_sentry_transaction_envelope(const nlohmann::json &event_json);

  // Multi-item Sentry envelope: one header line shared by all events, then a
  // "type": "transaction" item header + JSON payload pair per event. Lets a
  // whole Jaeger batch reach GlitchTip in a single POST instead of N.
  static std::string
  build_sentry_envelope(const std::vector<nlohmann::json> &events);

  JaegerLogger(const std::string &endpoint, prometheus::Counter &spans_sent,
               prometheus::Counter &spans_failed, prometheus::Gauge &queue_size,
               prometheus::Gauge &last_send_duration,
               prometheus::Histogram &send_latency,
               prometheus::Histogram &queue_time,
               size_t batch_size = g_tracing_default_batch_size,
               int flush_interval_ms = g_tracing_send_interval_ms,
               double sample_rate = 1.0,
               const std::string &sentry_dsn = "",
               const std::string &sentry_service = "",
               const std::string &sentry_environment = "",
               const std::string &sentry_release = "",
               prometheus::Counter *sentry_spans_sent = nullptr,
               prometheus::Counter *sentry_spans_failed = nullptr,
               double sentry_sample_rate = 1.0,
               TracingBreakerSettings breaker_settings = {});
  ~JaegerLogger();

  std::string generate_trace_id();
  std::string generate_span_id();

  // W3C Trace Context helpers
  std::string generate_traceparent(std::string_view trace_id,
                                   std::string_view span_id,
                                   bool sampled = true);

  // Parse traceparent header: "00-{trace-id}-{parent-id}-{flags}"
  static bool parse_traceparent(std::string_view traceparent,
                                std::string &trace_id,
                                std::string &parent_span_id, bool &sampled);

  void enqueue_span(const std::string &trace_id, const std::string &span_id,
                    const std::string &parent_id, const std::string &name,
                    uint64_t start_us, uint64_t end_us,
                    const std::string &service_name,
                    const nlohmann::json &attributes = {});

  void log_request(const std::string &method, const std::string &url,
                   int status_code, uint64_t start_us, uint64_t end_us,
                   const std::string &service_name,
                   const std::string &request_id = "",
                   const std::string &trace_id = "",
                   const std::string &span_id = "",
                   const std::string &parent_id = "",
                   const nlohmann::json &additional_attributes = {},
                   const std::string &name_override = "");

  // Simplified validation
  static bool validate_traceparent(std::string_view traceparent);

  void sender_loop(std::stop_token st);
  bool send_span(const std::string &trace_id, const std::string &span_id,
                 const std::string &parent_id, const std::string &name,
                 uint64_t start_us, uint64_t end_us,
                 const std::string &service_name,
                 const nlohmann::json &attributes = {});

  bool should_sample() const;

  // Check if sampling should be applied (with error flag - always sample
  // errors)
  bool should_sample(bool is_error) const;
};

#endif // TRACE_LOGGER_HPP

#ifndef APP_CONTEXT_HPP
#define APP_CONTEXT_HPP

#include "config.hpp"
#include "in_flight_tracker.hpp"
#include "metrics_history.hpp"
#include <memory>
#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string>
#include <vector>

struct ProxyMetrics {
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) -
  // Intentional design for Prometheus metrics
  prometheus::Counter &m_client_requests;
  prometheus::Counter &m_nats_requests;
  prometheus::Counter &m_client_errors;
  prometheus::Counter &m_nats_errors;
  prometheus::Counter &m_nats_connection_creates;
  prometheus::Counter &m_nats_connection_errors;
  prometheus::Histogram &m_nats_request_duration_seconds;
  prometheus::Counter &m_bytes_received;
  prometheus::Counter &m_bytes_sent;
  prometheus::Histogram &m_request_duration_seconds;
  // Request/Response size histograms
  prometheus::Histogram &m_request_size_bytes;
  prometheus::Histogram &m_response_size_bytes;
  // Duplicate detection metric
  prometheus::Counter &m_duplicate_requests_total;
  // Duplicate POST requests from clients (proxy-side detection)
  prometheus::Counter &m_duplicate_posts_detected;
  // HTTP responses by status code (per-HTTP-status breakdown for error-rate /
  // SLO panels). Labeled family: series created lazily on every response.
  prometheus::Family<prometheus::Counter> &m_responses_total;
  // HTTP DB Gateway (/v1/sql/*) metrics. Labeled families: series are created
  // lazily by label set (db, type, status), so recording code just calls
  // Add() with the label map on every request.
  prometheus::Family<prometheus::Counter> &m_db_requests_total;
  prometheus::Family<prometheus::Histogram> &m_db_request_duration_seconds;
  prometheus::Family<prometheus::Histogram> &m_db_nats_request_duration_seconds;
  // In-flight HTTP requests (saturation / backpressure visibility).
  prometheus::Gauge &m_in_flight_requests;
  // NATS connection state (1 = connected, 0 = disconnected) for availability
  // panels and alerting.
  prometheus::Gauge &m_nats_connected;
  // Readiness state (1 = ready, 0 = not ready) mirrored from /health/ready.
  prometheus::Gauge &m_health_ready;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

struct TracingMetrics {
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) -
  // Intentional design for Prometheus metrics
  prometheus::Counter &m_spans_sent;
  prometheus::Counter &m_spans_failed;
  prometheus::Gauge &m_queue_size;
  prometheus::Gauge &m_last_send_duration;
  prometheus::Histogram &m_send_latency;
  prometheus::Histogram &m_queue_time;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

struct WorkerMetrics {
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) -
  // Intentional design for Prometheus metrics
  prometheus::Counter &m_requests_processed;
  prometheus::Counter &m_l2_calls;
  prometheus::Counter &m_l2_errors;
  prometheus::Counter &m_bytes_received;
  prometheus::Counter &m_bytes_sent;
  prometheus::Histogram &m_request_duration_seconds;
  prometheus::Histogram &m_l2_call_duration_seconds;
  prometheus::Counter &m_processing_json_errors;
  prometheus::Counter &m_processing_validation_errors;
  prometheus::Histogram &m_l2_response_size_bytes;
  prometheus::Gauge &m_circuit_breaker_state;
  prometheus::Counter &m_duplicate_requests;
  // HTTP DB Gateway metrics: execution counters/duration on the worker and
  // per-database pool gauges (state label: "active"/"idle"). Labeled families
  // as in ProxyMetrics.
  prometheus::Family<prometheus::Counter> &m_db_requests_total;
  prometheus::Family<prometheus::Histogram> &m_db_query_duration_seconds;
  prometheus::Family<prometheus::Gauge> &m_db_pool_connections;
  // HTTP responses sent back over NATS by status code (per-status breakdown).
  prometheus::Family<prometheus::Counter> &m_responses_total;
  // In-flight requests currently processed by the worker (saturation).
  prometheus::Gauge &m_in_flight_requests;
  // Worker thread-pool queue depth (backpressure early warning).
  prometheus::Gauge &m_queue_size;
  // NATS connection state (1 = connected, 0 = disconnected) for availability.
  prometheus::Gauge &m_nats_connected;
  // Readiness state (1 = ready, 0 = not ready) mirrored from /health/ready.
  prometheus::Gauge &m_health_ready;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

struct ServerMetrics {
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) -
  // Intentional design for Prometheus metrics
  prometheus::Counter &m_requests;
  prometheus::Counter &m_request_errors;
  prometheus::Counter &m_bytes_received;
  prometheus::Counter &m_bytes_sent;
  prometheus::Histogram &m_request_duration_seconds;
  // HTTP responses by status code (per-HTTP-status breakdown).
  prometheus::Family<prometheus::Counter> &m_responses_total;
  // Readiness state (1 = ready, 0 = not ready) mirrored from /health/ready.
  prometheus::Gauge &m_health_ready;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

struct HttpPoolMetrics {
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) -
  // Intentional design for Prometheus metrics
  prometheus::Gauge &m_active_clients;
  prometheus::Gauge &m_available_clients;
  prometheus::Counter &m_client_acquisitions_total;
  prometheus::Counter &m_client_releases_total;
  prometheus::Counter &m_stale_evictions_total;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

struct RateLimiterMetrics {
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) -
  // Intentional design for Prometheus metrics
  prometheus::Gauge &m_available_tokens;
  prometheus::Counter &m_rejected_requests;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

struct PerIPRateLimiterMetrics {
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) -
  // Intentional design for Prometheus metrics
  prometheus::Counter &m_rejected_requests;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

struct InternalMemoryMetrics {
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) -
  // Intentional design for Prometheus metrics
  prometheus::Gauge &m_per_ip_rate_limiter_ips_tracked;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

class JaegerLogger;
class NatsClient;
class RateLimiter;
class PerIPRateLimiter;
class LabeledCounterCollector;
class LabeledHistogramCollector;
class DuplicateDetector;

// Sub-contexts for logical grouping of components by mode
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct ProxyContext {
  std::unique_ptr<ProxyMetrics> m_metrics;
  std::unique_ptr<HttpPoolMetrics> m_http_pool_metrics;
  std::unique_ptr<RateLimiterMetrics> m_rate_limiter_metrics;
  std::unique_ptr<PerIPRateLimiterMetrics> m_per_ip_rate_limiter_metrics;
  std::unique_ptr<RateLimiter> m_rate_limiter;
  std::unique_ptr<PerIPRateLimiter> m_per_ip_rate_limiter;
  // Dynamic-label collectors: one instance per label. Per-IP is fed by a
  // snapshot provider reading the rate limiter; per-client-id by direct
  // recording from the request handler. The per-client latency histogram uses
  // the same X-DataHub-Client-Id label for Grafana p95-per-client panels.
  std::shared_ptr<LabeledCounterCollector> m_per_ip_metrics_collector;
  std::shared_ptr<LabeledCounterCollector> m_per_client_id_metrics_collector;
  std::shared_ptr<LabeledHistogramCollector> m_per_client_id_latency_collector;
  // Duplicate POST bodies detected per X-DataHub-Client-Id header (fed by the
  // request handler; counts the same-body-hash-seen-again deliveries per
  // client). Powers the "top duplicate clients" Grafana panel.
  std::shared_ptr<LabeledCounterCollector> m_per_client_id_duplicate_collector;
  // Detects duplicate POST bodies from clients (keyed by SHA-256), keeps a
  // bounded report of the top duplicates; served on GET /debug/duplicates.
  std::unique_ptr<DuplicateDetector> m_duplicate_detector;
  std::unique_ptr<InternalMemoryMetrics> m_internal_memory_metrics;
};

struct WorkerContext {
  std::unique_ptr<WorkerMetrics> m_metrics;
};

struct ServerContext {
  std::unique_ptr<ServerMetrics> m_metrics;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
class AppContext {
public:
  Config m_config;
  std::unique_ptr<JaegerLogger> m_tracer;
  std::shared_ptr<prometheus::Registry> m_proxy_registry;
  std::shared_ptr<prometheus::Registry> m_worker_registry;
  std::shared_ptr<prometheus::Registry> m_server_registry;
  std::unique_ptr<TracingMetrics> m_tracing_metrics;
  std::shared_ptr<NatsClient> m_nats_client;
  InFlightTracker m_in_flight_tracker;

  // Sub-contexts
  ProxyContext m_proxy;
  WorkerContext m_worker;
  ServerContext m_server;

  // In-process ring buffers feeding the /stats sparklines. One sampler thread
  // per registry; cheap and independent of /stats request rate. Held by
  // unique_ptr because MetricsHistory is non-movable (atomic + thread).
  std::unique_ptr<MetricsHistory> m_proxy_stats_history;
  std::unique_ptr<MetricsHistory> m_worker_stats_history;
  std::unique_ptr<MetricsHistory> m_server_stats_history;

  AppContext();
  ~AppContext();
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

#endif // APP_CONTEXT_HPP

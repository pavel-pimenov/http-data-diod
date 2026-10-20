#include "app_context.hpp"
#include "db_query_utils.hpp"
#include "error_categorizer.hpp"
#include "logger.hpp"
#include "metrics_manager.hpp"
#include "tracing_metrics.hpp"
// The AppContext destructor is defined out of line: its unique_ptr members
// (see app_context.hpp) need complete types here, which include-cleaner cannot
// see through the unique_ptr default deleter, so keep these includes.
#include "rate_limiter.hpp"        // IWYU pragma: keep
#include "rate_limiter_per_ip.hpp" // IWYU pragma: keep
#include "duplicate_detector.hpp"  // IWYU pragma: keep
#include "sentry_client.hpp"
#include "trace_logger.hpp" // IWYU pragma: keep
#include <cstdlib>
#include <memory>
#include <prometheus/registry.h>

// Build version, defined in version.cpp. Declared here the same way config.cpp
// does it (crash_handler.hpp declares it too, but that header drags in the
// whole Sentry crash machinery).
extern const char *g_l2_proxy_version;

ProxyContext::~ProxyContext() = default;

AppContext::AppContext() {
  init_common();
  init_proxy_metrics();
  init_worker_metrics();
  init_server_metrics();

  m_sentry = std::make_unique<SentryClient>(
      m_config.m_sentry.m_dsn, m_sentry_metrics->m_events_sent,
      m_sentry_metrics->m_events_failed, m_sentry_metrics->m_queue_size,
      m_sentry_metrics->m_events_dropped, m_config.m_app.m_mode, m_config.m_sentry.m_environment, m_config.m_sentry.m_release,
      m_config.m_sentry.m_timeout_ms, m_config.m_sentry.m_max_queue_size);
}

AppContext::~AppContext() {
  stop_stats_history();
  m_tracer.reset();
}

void AppContext::stop_stats_history() const {
  for (auto *history : {m_proxy_stats_history.get(),
                        m_worker_stats_history.get(),
                        m_server_stats_history.get(),
                        m_common_stats_history.get()}) {
    if (history) {
      history->stop();
    }
  }
}

bool AppContext::is_proxy_components_initialized() const {
  return m_nats_client != nullptr &&
         m_proxy.m_duplicate_detector != nullptr;
}

void AppContext::init_common() {
  m_config.load_from_env();
  if (!m_config.validate()) {
    Logger::error("Configuration validation failed, exiting");
    std::exit(1);
  }
  Logger::set_level_from_string(m_config.m_app.m_log_level);
  m_proxy_registry = std::make_shared<prometheus::Registry>();
  m_worker_registry = std::make_shared<prometheus::Registry>();
  m_server_registry = std::make_shared<prometheus::Registry>();
  m_common_registry = std::make_shared<prometheus::Registry>();
  MetricRegistrar const reg_common(m_common_registry);

  m_proxy_stats_history = std::make_unique<MetricsHistory>(m_proxy_registry);
  m_worker_stats_history = std::make_unique<MetricsHistory>(m_worker_registry);
  m_server_stats_history = std::make_unique<MetricsHistory>(m_server_registry);
  m_common_stats_history = std::make_unique<MetricsHistory>(m_common_registry);
  m_proxy_stats_history->start();
  m_worker_stats_history->start();
  m_server_stats_history->start();
  m_common_stats_history->start();

  m_tracing_metrics = std::make_unique<TracingMetrics>(TracingMetrics{
      JaegerDeliveryMetrics{
          reg_common.counter(
              "l2_tracing_spans_sent_total",
              "Total number of spans sent to Jaeger"),
          reg_common.counter(
              "l2_tracing_spans_failed_total",
              "Total number of spans failed to send to Jaeger"),
          reg_common.gauge("l2_tracing_queue_size",
                           "Current size of the tracing span queue"),
          reg_common.gauge(
              "l2_tracing_last_send_duration_seconds",
              "Duration of the last span send to Jaeger"),
          reg_common.histogram(
              "l2_tracing_send_latency_seconds",
              "Histogram of span batch send latency in seconds",
              histogram_buckets::g_buckets.m_latency_ms_to_5s),
          reg_common.histogram(
              "l2_tracing_queue_time_seconds",
              "Histogram of time spans spend in queue before sending in seconds",
              histogram_buckets::g_buckets.m_latency_ms_to_5s),
          reg_common.counter_family(
              "l2_tracing_spans_dropped_total",
              "Total number of spans lost on the way to Jaeger, labeled by "
              "reason")},
      reg_common.counter(
          "l2_tracing_sentry_transactions_sent_total",
          "Total number of spans delivered to the Sentry/GlitchTip "
          "performance endpoint as transactions"),
      reg_common.counter(
          "l2_tracing_sentry_transactions_failed_total",
          "Total number of spans not delivered to the Sentry/GlitchTip "
          "performance endpoint"),
      reg_common.counter(
          "l2_tracing_traceparent_invalid_total",
          "Total number of client traceparent headers that could not be "
          "parsed")});

  // prometheus-cpp exports a labeled family only after the first Add(), and a
  // healthy stack never loses a span, so every reason gets its zero series up
  // front (same reasoning as the circuit-breaker transitions in the worker
  // registry).
  auto &spans_dropped = m_tracing_metrics->m_jaeger.m_spans_dropped;
  for (const char *reason : g_tracing_drop_reasons) {
    spans_dropped.Add({{"reason", reason}});
  }

  m_sentry_metrics = std::make_unique<SentryMetrics>(SentryMetrics{
      reg_common.counter(
          "l2_worker_sentry_events_sent_total",
          "Sentry events successfully delivered"),
      reg_common.counter(
          "l2_worker_sentry_events_failed_total",
          "Sentry events that failed to deliver or were dropped while the "
          "queue was full"),
      reg_common.gauge("l2_worker_sentry_queue_size",
                       "Sentry events pending in the async queue"),
      reg_common.counter_family(
          "l2_worker_sentry_events_dropped_total",
          "Total number of Sentry events lost before a delivery attempt, "
          "labeled by reason")});

  // prometheus-cpp exports a labeled family only after the first Add(), and a
  // healthy stack loses no Sentry event, so the reason gets its zero series up
  // front.
  m_sentry_metrics->m_events_dropped.Add(
      {{"reason", sentry::kDropReasonQueueFull}});

  // The HTTP pool is shared: the worker uses it for the L2 calls and the
  // tracer for the span delivery, so its metrics belong to the common registry
  // (before this they were registered on the proxy's, where the worker's pool
  // numbers never showed up).
  m_proxy.m_http_pool_metrics =
      std::make_unique<HttpPoolMetrics>(HttpPoolMetrics{
          reg_common.gauge(
              "l2_http_pool_active_clients",
              "Current number of active HTTP/SSL clients in the pool"),
          reg_common.gauge(
              "l2_http_pool_available_clients",
              "Current number of available HTTP/SSL clients in the pool"),
          reg_common.counter(
              "l2_http_pool_client_acquisitions_total",
              "Total number of HTTP client acquisitions from the pool"),
          reg_common.counter(
              "l2_http_pool_client_releases_total",
              "Total number of HTTP client releases to the pool"),
          reg_common.counter(
              "l2_http_pool_stale_evictions_total",
              "Total number of stale HTTP connections evicted from the pool"),
          reg_common.counter(
              "l2_http_pool_client_acquisition_timeouts_total",
              "Total number of HTTP pool acquire timeouts (all connections in "
              "use)"),
          reg_common.histogram(
              "l2_http_pool_client_acquisition_duration_seconds",
              "Time an HTTP pool acquire waited for a free connection",
              histogram_buckets::g_buckets.m_latency_ms_to_5s)});

  // Deployment identity: the value is always 1, the labels carry the build
  // version and the mode, so a dashboard (or an alert) can tell which build an
  // instance runs without relabeling gymnastics. The common registry is
  // exported by every mode, so the series exists in proxy, worker and server.
  reg_common.gauge_family("l2_common_build_info",
                          "Build information (value is always 1)")
      .Add({{"version", g_l2_proxy_version}, {"mode", m_config.m_app.m_mode}})
      .Set(1);
}

void AppContext::init_proxy_metrics() {
  MetricRegistrar const reg_proxy(m_proxy_registry);
  m_proxy.m_metrics = std::make_unique<ProxyMetrics>(ProxyMetrics{
      reg_proxy.counter(
          "l2_proxy_client_requests_total",
          "Total number of client requests received"),
      reg_proxy.counter("l2_proxy_nats_requests_total",
                        "Total number of NATS requests sent"),
      reg_proxy.counter_family(
          "l2_proxy_client_request_errors_total",
          "Total number of client request errors, labeled by error type "
          "(see the README catalogue for the meaning of each type)"),
      reg_proxy.counter_family(
          "l2_proxy_rejections_total",
          "Total number of requests refused by the proxy, labeled by reason "
          "(see the README catalogue for the meaning of each reason)"),
      reg_proxy.counter("l2_proxy_nats_errors_total",
                        "Total number of NATS operation errors"),
      reg_proxy.counter(
          "l2_proxy_nats_connection_creates_total",
          "Total number of NATS connections created"),
      reg_proxy.counter("l2_proxy_nats_connection_errors_total",
                        "Total number of NATS connection errors"),
      reg_proxy.counter("l2_proxy_nats_timeouts_total",
                        "NATS request/reply timeouts"),
      reg_proxy.counter("l2_proxy_nats_reconnects_total",
                        "Successful NATS reconnects after a connection loss"),
      reg_proxy.gauge("l2_proxy_nats_subscriptions",
                      "Number of active NATS subscriptions"),
      reg_proxy.histogram(
          "l2_proxy_nats_request_duration_seconds",
          "Histogram of NATS request duration in seconds",
          histogram_buckets::g_buckets.m_latency_ms_to_10s),
      reg_proxy.counter(
          "l2_proxy_nats_poll_attempts_total",
          "Total number of NATS request/reply attempts made by the proxy "
          "poll loop (1 per request means a plain round-trip, more means "
          "retries after an empty/lost reply)"),
      reg_proxy.histogram(
          "l2_proxy_nats_poll_attempt_duration_seconds",
          "Histogram of a single NATS request/reply attempt duration in "
          "seconds inside the proxy poll loop",
          histogram_buckets::g_buckets.m_latency_100us_to_1s),
      reg_proxy.histogram(
          "l2_proxy_nats_poll_retry_wait_seconds",
          "Histogram of the backoff sleep between NATS poll attempts in "
          "seconds (non-zero only when a reply was lost or empty)",
          histogram_buckets::g_buckets.m_latency_ms_to_10s),
      reg_proxy.histogram(
          "l2_proxy_task_queue_wait_seconds",
          "Histogram of the time an HTTP request spent waiting in the proxy "
          "task queue before a worker thread started processing it",
          histogram_buckets::g_buckets.m_latency_100us_to_1s),
      reg_proxy.counter(
          "l2_proxy_task_queue_enqueued_total",
          "Total number of tasks accepted by the proxy task queue"),
      reg_proxy.counter(
          "l2_proxy_task_queue_rejected_total",
          "Total number of tasks rejected by the proxy task queue "
          "(queue limit reached, request served with an error)"),
      reg_proxy.counter(
          "l2_proxy_bytes_received_total",
          "Total number of bytes received from clients"),
      reg_proxy.counter("l2_proxy_bytes_sent_total",
                        "Total number of bytes sent to clients"),
      reg_proxy.histogram(
          "l2_proxy_request_duration_seconds",
          "Histogram of request processing duration in seconds",
          histogram_buckets::g_buckets.m_latency_5ms_to_10s),
      reg_proxy.histogram(
          "l2_proxy_request_size_bytes",
          "Histogram of client request sizes in bytes",
          histogram_buckets::g_buckets.m_size_100b_to_5mb),
      reg_proxy.histogram("l2_proxy_response_size_bytes",
                          "Histogram of response sizes in bytes",
                          histogram_buckets::g_buckets.m_size_100b_to_5mb),
      reg_proxy.counter(
          "l2_proxy_duplicate_requests_total",
          "Total number of NATS request/reply re-sends by the proxy after "
          "losing the first response (e.g. NATS reconnect)"),
      reg_proxy.counter(
          "l2_proxy_duplicate_posts_detected_total",
          "Total number of duplicate POST bodies from clients detected by "
          "the proxy (same body hash seen more than once)"),
      reg_proxy.counter_family(
          "l2_proxy_responses_total",
          "Total number of proxy HTTP responses by HTTP status code"),
      reg_proxy.counter_family(
          "l2_proxy_db_requests_total",
          "Total number of HTTP DB Gateway requests by database, type and "
          "HTTP status"),
      reg_proxy.histogram_family(
          "l2_proxy_db_request_duration_seconds",
          "Histogram of HTTP DB Gateway request processing duration in "
          "seconds by database"),
      reg_proxy.histogram_family(
          "l2_proxy_db_nats_request_duration_seconds",
          "Histogram of HTTP DB Gateway NATS round-trip duration in seconds "
          "by database"),
      reg_proxy.gauge(
          "l2_proxy_in_flight_requests",
          "Current number of in-flight proxy HTTP requests"),
      reg_proxy.gauge(
          "l2_proxy_nats_connected",
          "NATS connection state (1 = connected, 0 = disconnected)"),
      reg_proxy.gauge(
          "l2_proxy_health_ready",
          "Readiness state (1 = ready, 0 = not ready) mirrored from "
          "/health/ready"),
      reg_proxy.gauge(
          "l2_proxy_duplicate_tracked_clients",
          "Current number of client ids tracked by the duplicate detector "
          "(bounded by DUPLICATE_DETECTION_MAX_CLIENTS)")});

  // prometheus-cpp exports a labeled family only after the first Add(), and a
  // healthy proxy refuses nothing, so every enum value gets its zero series up
  // front (same reasoning as the circuit-breaker transitions and the L2 call
  // errors in the worker registry).
  auto &client_errors = m_proxy.m_metrics->m_client_errors;
  for (const char *type : error_categorizer::g_http_error_names) {
    client_errors.Add({{"type", type}});
  }

  // Same for the refusals: a healthy proxy refuses nothing, so every reason
  // gets its zero series up front.
  auto &rejections = m_proxy.m_metrics->m_rejections;
  for (const char *reason : error_categorizer::g_rejection_reason_names) {
    rejections.Add({{"reason", reason}});
  }
}

void AppContext::init_worker_metrics() {
  MetricRegistrar const reg_worker(m_worker_registry);
  m_worker.m_metrics = std::make_unique<WorkerMetrics>(WorkerMetrics{
      reg_worker.counter(
          "l2_worker_requests_processed_total",
          "Total number of requests processed by L2 worker"),
      reg_worker.counter(
          "l2_worker_l2_calls_total",
          "Total number of L2 server calls made by worker"),
      reg_worker.counter(
          "l2_worker_l2_errors_total",
          "Total number of L2 server call errors in worker"),
      reg_worker.counter_family(
          "l2_worker_retries_total",
          "L2 call retries split by outcome (retried/exhausted)"),
      reg_worker.counter_family(
          "l2_worker_l2_call_errors_total",
          "L2 server call errors by categorized cause (type label)"),
      reg_worker.counter("l2_worker_bytes_received_total",
                         "Total number of bytes received"),
      reg_worker.counter("l2_worker_bytes_sent_total",
                         "Total number of bytes sent"),
      reg_worker.histogram(
          "l2_worker_request_duration_seconds",
          "Histogram of request processing duration in seconds",
          histogram_buckets::g_buckets.m_latency_5ms_to_10s),
      reg_worker.histogram(
          "l2_worker_l2_call_duration_seconds",
          "Histogram of L2 server call duration in seconds",
          histogram_buckets::g_buckets.m_latency_5ms_to_10s),
      reg_worker.counter(
          "l2_worker_processing_json_errors_total",
          "JSON parsing errors during request processing"),
      reg_worker.counter(
          "l2_worker_processing_validation_errors_total",
          "Request validation errors"),
      reg_worker.histogram(
          "l2_worker_l2_response_size_bytes",
          "Histogram of L2 response sizes in bytes",
          histogram_buckets::g_buckets.m_size_100b_to_5mb),
      reg_worker.gauge(
          "l2_worker_circuit_breaker_state",
          "Circuit breaker state (0=closed, 1=open, 2=half_open)"),
      reg_worker.counter_family(
          "l2_worker_circuit_breaker_transitions_total",
          "Total number of circuit breaker transitions by target state "
          "(open/half_open/closed)"),
      reg_worker.counter(
          "l2_worker_circuit_breaker_rejected_total",
          "Total number of requests fast-failed by the open circuit breaker"),
      reg_worker.counter(
          "l2_worker_duplicate_requests_total",
          "Total number of duplicate NATS requests served from dedup cache"),
      reg_worker.counter_family(
          "l2_worker_db_requests_total",
          "Total number of HTTP DB Gateway requests executed by the worker "
          "by database, type and HTTP status"),
      reg_worker.counter_family(
          "l2_worker_db_query_errors_total",
          "Failed DB queries broken down by error class (the SQLSTATE class "
          "for PostgreSQL)"),
      reg_worker.histogram_family(
          "l2_worker_db_query_duration_seconds",
          "Histogram of DB query execution duration in seconds by database"),
      reg_worker.gauge_family(
          "l2_worker_db_pool_connections",
          "Current number of DB pool connections by database and state "
          "(active/idle)"),
      reg_worker.gauge_family(
          "l2_worker_db_pool_capacity",
          "Configured maximum DB pool connections per database"),
      reg_worker.counter_family(
          "l2_worker_db_pool_exhausted_total",
          "Total DB pool acquire requests rejected because every configured "
          "connection was busy (pool saturated)"),
      reg_worker.histogram_family(
          "l2_worker_db_pool_acquire_duration_seconds",
          "Histogram of DB pool connection acquire duration in seconds "
          "(pooled hand-out or fresh connect)"),
      reg_worker.gauge_family(
          "l2_worker_db_gateway_ready",
          "DB Gateway readiness per configured database (1 = executor live, "
          "0 = still starting up)"),
      reg_worker.counter_family(
          "l2_worker_responses_total",
          "Total number of worker NATS responses by HTTP status code"),
      reg_worker.gauge(
          "l2_worker_in_flight_requests",
          "Current number of in-flight worker requests"),
      reg_worker.gauge(
          "l2_worker_queue_size",
          "Current worker thread-pool queue depth"),
      reg_worker.gauge(
          "l2_worker_queue_capacity",
          "Capacity of the worker thread-pool queue (denominator for the "
          "queue depth)"),
      reg_worker.gauge(
          "l2_worker_nats_connected",
          "NATS connection state (1 = connected, 0 = disconnected)"),
      reg_worker.gauge(
          "l2_worker_health_ready",
          "Readiness state (1 = ready, 0 = not ready) mirrored from "
          "/health/ready"),
      reg_worker.gauge(
          "l2_worker_graceful_shutdown_seconds",
          "Last graceful-shutdown drain duration in seconds (time from "
          "SIGTERM to full shutdown, 0 while running)")});

  // Materialize the breaker's bounded transition series up front: prometheus-cpp
  // exports a labeled family only after the first Add(), and a healthy stack may
  // never trip the breaker, so without this the family would be absent from
  // /metrics (and the golden check) until the first transition.
  auto &cb_transitions =
      m_worker.m_metrics->m_circuit_breaker_transitions_total;
  cb_transitions.Add({{"to", "open"}});
  cb_transitions.Add({{"to", "half_open"}});
  cb_transitions.Add({{"to", "closed"}});

  // Same reasoning for the categorized L2 call errors: a healthy stack never
  // fails an L2 call, so every enum value gets its zero series up front.
  auto &l2_call_errors = m_worker.m_metrics->m_l2_call_errors_total;
  for (const char *type : error_categorizer::g_l2_error_names) {
    l2_call_errors.Add({{"type", type}});
  }

  // Same for the DB error classes: the family must exist before the first
  // failed query, otherwise it would not be exported at all.
  auto &db_errors = m_worker.m_metrics->m_db_query_errors_total;
  for (const char *error_class : g_db_error_classes) {
    db_errors.Add({{"error_class", error_class}});
  }

  // Same for the retry outcomes.
  auto &retries = m_worker.m_metrics->m_retries_total;
  for (const char *outcome : g_worker_retry_outcomes) {
    retries.Add({{"outcome", outcome}});
  }
}

void AppContext::init_server_metrics() {
  MetricRegistrar const reg_server(m_server_registry);
  m_server.m_metrics = std::make_unique<ServerMetrics>(ServerMetrics{
      reg_server.counter(
          "l2_server_requests_total",
          "Total number of requests received by L2 server"),
      reg_server.counter(
          "l2_server_request_errors_total",
          "Total number of request errors in L2 server"),
      reg_server.counter(
          "l2_server_bytes_received_total",
          "Total number of bytes received by L2 server"),
      reg_server.counter("l2_server_bytes_sent_total",
                         "Total number of bytes sent by L2 server"),
      reg_server.histogram(
          "l2_server_request_duration_seconds",
          "Histogram of request processing duration in seconds",
          histogram_buckets::g_buckets.m_latency_5ms_to_10s),
      reg_server.counter_family(
          "l2_server_responses_total",
          "Total number of L2 server HTTP responses by HTTP status code"),
      reg_server.gauge(
          "l2_server_health_ready",
          "Readiness state (1 = ready, 0 = not ready) mirrored from "
          "/health/ready")});
}

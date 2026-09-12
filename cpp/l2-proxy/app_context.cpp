#include "app_context.hpp"
#include "logger.hpp"
#include "metrics_manager.hpp"
#include "sentry_client.hpp"
#include "trace_logger.hpp"
#include <cstdlib>

AppContext::AppContext() {
  init_common();
  init_proxy_metrics();
  init_worker_metrics();
  init_server_metrics();

  m_sentry = std::make_unique<SentryClient>(
      m_config.m_sentry_dsn, m_sentry_metrics->m_events_sent,
      m_sentry_metrics->m_events_failed, m_sentry_metrics->m_queue_size,
      m_config.m_mode, m_config.m_sentry_environment, m_config.m_sentry_release,
      m_config.m_sentry_timeout_ms, m_config.m_sentry_max_queue_size);
}

AppContext::~AppContext() {
  if (m_proxy_stats_history) {
    m_proxy_stats_history->stop();
  }
  if (m_worker_stats_history) {
    m_worker_stats_history->stop();
  }
  if (m_server_stats_history) {
    m_server_stats_history->stop();
  }
  if (m_common_stats_history) {
    m_common_stats_history->stop();
  }
  m_tracer.reset();
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
  Logger::set_level_from_string(m_config.m_log_level);
  m_proxy_registry = std::make_shared<prometheus::Registry>();
  m_worker_registry = std::make_shared<prometheus::Registry>();
  m_server_registry = std::make_shared<prometheus::Registry>();
  m_common_registry = std::make_shared<prometheus::Registry>();

  m_proxy_stats_history = std::make_unique<MetricsHistory>(m_proxy_registry);
  m_worker_stats_history = std::make_unique<MetricsHistory>(m_worker_registry);
  m_server_stats_history = std::make_unique<MetricsHistory>(m_server_registry);
  m_common_stats_history = std::make_unique<MetricsHistory>(m_common_registry);
  m_proxy_stats_history->start();
  m_worker_stats_history->start();
  m_server_stats_history->start();
  m_common_stats_history->start();

  m_tracing_metrics = std::make_unique<TracingMetrics>(TracingMetrics{
      MetricsManager::create_counter(
          m_common_registry, "l2_tracing_spans_sent_total",
          "Total number of spans sent to Jaeger"),
      MetricsManager::create_counter(
          m_common_registry, "l2_tracing_spans_failed_total",
          "Total number of spans failed to send to Jaeger"),
      MetricsManager::create_gauge(m_common_registry, "l2_tracing_queue_size",
                                   "Current size of the tracing span queue"),
      MetricsManager::create_gauge(
          m_common_registry, "l2_tracing_last_send_duration_seconds",
          "Duration of the last span send to Jaeger"),
      MetricsManager::create_histogram(
          m_common_registry, "l2_tracing_send_latency_seconds",
          "Histogram of span batch send latency in seconds",
          histogram_buckets::g_k_latency_ms_to_5s),
      MetricsManager::create_histogram(
          m_common_registry, "l2_tracing_queue_time_seconds",
          "Histogram of time spans spend in queue before sending in seconds",
          histogram_buckets::g_k_latency_ms_to_5s)});

  m_sentry_metrics = std::make_unique<SentryMetrics>(SentryMetrics{
      MetricsManager::create_counter(
          m_common_registry, "l2_worker_sentry_events_sent_total",
          "Sentry events successfully delivered"),
      MetricsManager::create_counter(
          m_common_registry, "l2_worker_sentry_events_failed_total",
          "Sentry events that failed to deliver or were dropped while the "
          "queue was full"),
      MetricsManager::create_gauge(m_common_registry,
                                   "l2_worker_sentry_queue_size",
                                   "Sentry events pending in the async queue")});
}

void AppContext::init_proxy_metrics() {
  m_proxy.m_metrics = std::make_unique<ProxyMetrics>(ProxyMetrics{
      MetricsManager::create_counter(
          m_proxy_registry, "l2_proxy_client_requests_total",
          "Total number of client requests received"),
      MetricsManager::create_counter(m_proxy_registry,
                                     "l2_proxy_nats_requests_total",
                                     "Total number of NATS requests sent"),
      MetricsManager::create_counter(m_proxy_registry,
                                     "l2_proxy_client_request_errors_total",
                                     "Total number of client request errors"),
      MetricsManager::create_counter(m_proxy_registry,
                                     "l2_proxy_nats_errors_total",
                                     "Total number of NATS operation errors"),
      MetricsManager::create_counter(
          m_proxy_registry, "l2_proxy_nats_connection_creates_total",
          "Total number of NATS connections created"),
      MetricsManager::create_counter(m_proxy_registry,
                                     "l2_proxy_nats_connection_errors_total",
                                     "Total number of NATS connection errors"),
      MetricsManager::create_histogram(
          m_proxy_registry, "l2_proxy_nats_request_duration_seconds",
          "Histogram of NATS request duration in seconds",
          histogram_buckets::g_k_latency_ms_to_10s),
      MetricsManager::create_counter(
          m_proxy_registry, "l2_proxy_bytes_received_total",
          "Total number of bytes received from clients"),
      MetricsManager::create_counter(m_proxy_registry,
                                     "l2_proxy_bytes_sent_total",
                                     "Total number of bytes sent to clients"),
      MetricsManager::create_histogram(
          m_proxy_registry, "l2_proxy_request_duration_seconds",
          "Histogram of request processing duration in seconds",
          histogram_buckets::g_k_latency_5ms_to_10s),
      MetricsManager::create_histogram(
          m_proxy_registry, "l2_proxy_request_size_bytes",
          "Histogram of client request sizes in bytes",
          histogram_buckets::g_k_size_100b_to_5mb),
      MetricsManager::create_histogram(m_proxy_registry,
                                       "l2_proxy_response_size_bytes",
                                       "Histogram of response sizes in bytes",
                                       histogram_buckets::g_k_size_100b_to_5mb),
      MetricsManager::create_counter(
          m_proxy_registry, "l2_proxy_duplicate_requests_total",
          "Total number of NATS request/reply re-sends by the proxy after "
          "losing the first response (e.g. NATS reconnect)"),
      MetricsManager::create_counter(
          m_proxy_registry, "l2_proxy_duplicate_posts_detected_total",
          "Total number of duplicate POST bodies from clients detected by "
          "the proxy (same body hash seen more than once)"),
      MetricsManager::create_counter_family(
          m_proxy_registry, "l2_proxy_responses_total",
          "Total number of proxy HTTP responses by HTTP status code"),
      MetricsManager::create_counter_family(
          m_proxy_registry, "l2_proxy_db_requests_total",
          "Total number of HTTP DB Gateway requests by database, type and "
          "HTTP status"),
      MetricsManager::create_histogram_family(
          m_proxy_registry, "l2_proxy_db_request_duration_seconds",
          "Histogram of HTTP DB Gateway request processing duration in "
          "seconds by database",
          histogram_buckets::g_k_latency_ms_to_10s),
      MetricsManager::create_histogram_family(
          m_proxy_registry, "l2_proxy_db_nats_request_duration_seconds",
          "Histogram of HTTP DB Gateway NATS round-trip duration in seconds "
          "by database",
          histogram_buckets::g_k_latency_ms_to_10s),
      MetricsManager::create_gauge(
          m_proxy_registry, "l2_proxy_in_flight_requests",
          "Current number of in-flight proxy HTTP requests"),
      MetricsManager::create_gauge(
          m_proxy_registry, "l2_proxy_nats_connected",
          "NATS connection state (1 = connected, 0 = disconnected)"),
      MetricsManager::create_gauge(
          m_proxy_registry, "l2_proxy_health_ready",
          "Readiness state (1 = ready, 0 = not ready) mirrored from "
          "/health/ready"),
      MetricsManager::create_gauge(
          m_proxy_registry, "l2_proxy_duplicate_tracked_clients",
          "Current number of client ids tracked by the duplicate detector "
          "(bounded by DUPLICATE_DETECTION_MAX_CLIENTS)")});

  m_proxy.m_http_pool_metrics =
      std::make_unique<HttpPoolMetrics>(HttpPoolMetrics{
          MetricsManager::create_gauge(
              m_proxy_registry, "l2_http_pool_active_clients",
              "Current number of active HTTP/SSL clients in the pool"),
          MetricsManager::create_gauge(
              m_proxy_registry, "l2_http_pool_available_clients",
              "Current number of available HTTP/SSL clients in the pool"),
          MetricsManager::create_counter(
              m_proxy_registry, "l2_http_pool_client_acquisitions_total",
              "Total number of HTTP client acquisitions from the pool"),
          MetricsManager::create_counter(
              m_proxy_registry, "l2_http_pool_client_releases_total",
              "Total number of HTTP client releases to the pool"),
          MetricsManager::create_counter(
              m_proxy_registry, "l2_http_pool_stale_evictions_total",
              "Total number of stale HTTP connections evicted from the pool")});
}

void AppContext::init_worker_metrics() {
  m_worker.m_metrics = std::make_unique<WorkerMetrics>(WorkerMetrics{
      MetricsManager::create_counter(
          m_worker_registry, "l2_worker_requests_processed_total",
          "Total number of requests processed by L2 worker"),
      MetricsManager::create_counter(
          m_worker_registry, "l2_worker_l2_calls_total",
          "Total number of L2 server calls made by worker"),
      MetricsManager::create_counter(
          m_worker_registry, "l2_worker_l2_errors_total",
          "Total number of L2 server call errors in worker"),
      MetricsManager::create_counter(m_worker_registry,
                                     "l2_worker_bytes_received_total",
                                     "Total number of bytes received"),
      MetricsManager::create_counter(m_worker_registry,
                                     "l2_worker_bytes_sent_total",
                                     "Total number of bytes sent"),
      MetricsManager::create_histogram(
          m_worker_registry, "l2_worker_request_duration_seconds",
          "Histogram of request processing duration in seconds",
          histogram_buckets::g_k_latency_5ms_to_10s),
      MetricsManager::create_histogram(
          m_worker_registry, "l2_worker_l2_call_duration_seconds",
          "Histogram of L2 server call duration in seconds",
          histogram_buckets::g_k_latency_5ms_to_10s),
      MetricsManager::create_counter(
          m_worker_registry, "l2_worker_processing_json_errors_total",
          "JSON parsing errors during request processing"),
      MetricsManager::create_counter(
          m_worker_registry, "l2_worker_processing_validation_errors_total",
          "Request validation errors"),
      MetricsManager::create_histogram(
          m_worker_registry, "l2_worker_l2_response_size_bytes",
          "Histogram of L2 response sizes in bytes",
          histogram_buckets::g_k_size_100b_to_5mb),
      MetricsManager::create_gauge(
          m_worker_registry, "l2_worker_circuit_breaker_state",
          "Circuit breaker state (0=closed, 1=open, 2=half_open)"),
      MetricsManager::create_counter(
          m_worker_registry, "l2_worker_duplicate_requests_total",
          "Total number of duplicate NATS requests served from dedup cache"),
       MetricsManager::create_counter_family(
           m_worker_registry, "l2_worker_db_requests_total",
           "Total number of HTTP DB Gateway requests executed by the worker "
           "by database, type and HTTP status"),
       MetricsManager::create_histogram_family(
           m_worker_registry, "l2_worker_db_query_duration_seconds",
           "Histogram of DB query execution duration in seconds by database",
           histogram_buckets::g_k_latency_ms_to_10s),
       MetricsManager::create_gauge_family(
           m_worker_registry, "l2_worker_db_pool_connections",
           "Current number of DB pool connections by database and state "
           "(active/idle)"),
       MetricsManager::create_gauge_family(
           m_worker_registry, "l2_worker_db_gateway_ready",
           "DB Gateway readiness per configured database (1 = executor live, "
           "0 = still starting up)"),
       MetricsManager::create_counter_family(
           m_worker_registry, "l2_worker_responses_total",
           "Total number of worker NATS responses by HTTP status code"),
       MetricsManager::create_gauge(
           m_worker_registry, "l2_worker_in_flight_requests",
           "Current number of in-flight worker requests"),
       MetricsManager::create_gauge(
           m_worker_registry, "l2_worker_queue_size",
           "Current worker thread-pool queue depth"),
       MetricsManager::create_gauge(
            m_worker_registry, "l2_worker_nats_connected",
            "NATS connection state (1 = connected, 0 = disconnected)"),
        MetricsManager::create_gauge(
            m_worker_registry, "l2_worker_health_ready",
            "Readiness state (1 = ready, 0 = not ready) mirrored from "
            "/health/ready"),
        MetricsManager::create_gauge(
            m_worker_registry, "l2_worker_graceful_shutdown_seconds",
            "Last graceful-shutdown drain duration in seconds (time from "
            "SIGTERM to full shutdown, 0 while running)")});
}

void AppContext::init_server_metrics() {
  m_server.m_metrics = std::make_unique<ServerMetrics>(ServerMetrics{
      MetricsManager::create_counter(
          m_server_registry, "l2_server_requests_total",
          "Total number of requests received by L2 server"),
      MetricsManager::create_counter(
          m_server_registry, "l2_server_request_errors_total",
          "Total number of request errors in L2 server"),
      MetricsManager::create_counter(
          m_server_registry, "l2_server_bytes_received_total",
          "Total number of bytes received by L2 server"),
      MetricsManager::create_counter(m_server_registry,
                                     "l2_server_bytes_sent_total",
                                     "Total number of bytes sent by L2 server"),
      MetricsManager::create_histogram(
          m_server_registry, "l2_server_request_duration_seconds",
          "Histogram of request processing duration in seconds",
          histogram_buckets::g_k_latency_5ms_to_10s),
      MetricsManager::create_counter_family(
          m_server_registry, "l2_server_responses_total",
          "Total number of L2 server HTTP responses by HTTP status code"),
      MetricsManager::create_gauge(
          m_server_registry, "l2_server_health_ready",
          "Readiness state (1 = ready, 0 = not ready) mirrored from "
          "/health/ready")});
}

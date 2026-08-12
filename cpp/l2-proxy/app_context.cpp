#include "app_context.hpp"
#include "duplicate_detector.hpp"
#include "labeled_counter_collector.hpp"
#include "labeled_histogram_collector.hpp"
#include "logger.hpp"
#include "metrics_manager.hpp"
#include "nats_client.hpp"
#include "rate_limiter.hpp"
#include "rate_limiter_per_ip.hpp"
#include "trace_logger.hpp"
#include <cstdlib>

AppContext::AppContext() {
  m_config.load_from_env();
  if (!m_config.validate()) {
    Logger::error("Configuration validation failed, exiting");
    std::exit(1);
  }
  Logger::set_level_from_string(m_config.m_log_level);
  m_proxy_registry = std::make_shared<prometheus::Registry>();
  m_worker_registry = std::make_shared<prometheus::Registry>();
  m_server_registry = std::make_shared<prometheus::Registry>();

  // Initialize tracing metrics (shared across modes)
  m_tracing_metrics = std::make_unique<TracingMetrics>(TracingMetrics{
      MetricsManager::create_counter(m_worker_registry,
                                     "l2_tracing_spans_sent_total",
                                     "Total number of spans sent to Jaeger"),
      MetricsManager::create_counter(
          m_worker_registry, "l2_tracing_spans_failed_total",
          "Total number of spans failed to send to Jaeger"),
      MetricsManager::create_gauge(m_worker_registry, "l2_tracing_queue_size",
                                   "Current size of the tracing span queue"),
      MetricsManager::create_gauge(m_worker_registry,
                                   "l2_tracing_last_send_duration_seconds",
                                   "Duration of the last span send to Jaeger"),
      MetricsManager::create_histogram(
          m_worker_registry, "l2_tracing_send_latency_seconds",
          "Histogram of span batch send latency in seconds",
          histogram_buckets::g_k_latency_ms_to_5s),
      MetricsManager::create_histogram(
          m_worker_registry, "l2_tracing_queue_time_seconds",
          "Histogram of time spans spend in queue before sending in seconds",
          histogram_buckets::g_k_latency_ms_to_5s)});

  // Initialize proxy metrics
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
          histogram_buckets::g_k_latency_ms_to_10s)});

  // Initialize worker metrics
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
          "(active/idle)")});

  // Initialize server metrics
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
          histogram_buckets::g_k_latency_5ms_to_10s)});

  // Initialize HTTP pool metrics
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

  // Initialize NATS client (proxy mode only; worker creates its own,
  // l2-server does not use NATS)
  if (m_config.m_mode == "proxy") {
    Logger::info("Using NATS for messaging (host={}:{}, subject={})",
                 m_config.m_nats_host, m_config.m_nats_port,
                 m_config.m_nats_subject);

    m_nats_client = std::make_shared<NatsClient>(m_config.create_nats_config());

    if (!m_nats_client->connect()) {
      Logger::error("Failed to connect to NATS server");
    } else {
      Logger::info("NATS client connected successfully");
    }
  }

  // Proxy-only components: rate limiters. They are not used in worker /
  // l2-server modes; in particular PerIPRateLimiter would otherwise run a
  // background cleanup thread to no effect.
  if (m_config.m_mode == "proxy") {
    // Initialize internal memory tracking metrics
    m_proxy.m_internal_memory_metrics = std::make_unique<InternalMemoryMetrics>(
        InternalMemoryMetrics{MetricsManager::create_gauge(
            m_proxy_registry, "l2_proxy_per_ip_rate_limiter_ips_tracked",
            "Current number of unique IPs tracked by per-IP rate limiter")});

    // Initialize global rate limiter metrics
    m_proxy.m_rate_limiter_metrics =
        std::make_unique<RateLimiterMetrics>(RateLimiterMetrics{
            MetricsManager::create_gauge(m_proxy_registry,
                                         "l2_rate_limiter_tokens",
                                         "Available rate limiter tokens"),
            MetricsManager::create_counter(
                m_proxy_registry, "l2_rate_limiter_rejected_total",
                "Total requests rejected by global rate limiter")});

    // Initialize per-IP rate limiter metrics
    m_proxy.m_per_ip_rate_limiter_metrics =
        std::make_unique<PerIPRateLimiterMetrics>(
            PerIPRateLimiterMetrics{MetricsManager::create_counter(
                m_proxy_registry, "l2_per_ip_rate_limiter_rejected_total",
                "Total requests rejected by per-IP rate limiter")});

    if (m_config.m_enable_global_rate_limiting) {
      m_proxy.m_rate_limiter = std::make_unique<RateLimiter>(
          static_cast<uint64_t>(m_config.m_global_max_tokens),
          static_cast<uint64_t>(m_config.m_global_refill_rate));
      Logger::info("Global rate limiter initialized: max={} tokens, "
                   "refill={}/sec",
                   m_config.m_global_max_tokens, m_config.m_global_refill_rate);
    } else {
      Logger::info(
          "Global rate limiter disabled (ENABLE_GLOBAL_RATE_LIMITING=false)");
    }

    // Initialize per-IP rate limiter
    if (m_config.m_enable_per_ip_rate_limiting) {
      m_proxy.m_per_ip_rate_limiter = std::make_unique<PerIPRateLimiter>(
          m_config.m_per_ip_max_tokens, m_config.m_per_ip_refill_rate,
          m_config.m_per_ip_max_ips, m_config.m_per_ip_cleanup_ttl_seconds);
      // Per-IP counters live in the rate limiter, so the collector is fed by a
      // snapshot provider that pulls the current IP set on every scrape.
      m_proxy.m_per_ip_metrics_collector = std::make_shared<
          LabeledCounterCollector>(
          "ip", "l2_proxy_per_ip_requests_total",
          "Total number of requests received per client IP",
          "l2_proxy_per_ip_rejected_total",
          "Total number of requests rejected by the per-IP rate limiter per "
          "client IP",
          [limiter = m_proxy.m_per_ip_rate_limiter.get()]() {
            std::vector<std::pair<std::string, LabeledCounterCollector::Stats>>
                entries;
            if (limiter != nullptr) {
              for (const auto &[ip, stats] : limiter->get_per_ip_stats()) {
                entries.emplace_back(
                    ip, LabeledCounterCollector::Stats{stats.m_requests,
                                                       stats.m_rejected});
              }
            }
            return entries;
          });
      Logger::info("Per-IP rate limiter initialized: max_tokens={} "
                   "refill_rate={} max_ips={} cleanup_ttl={}s",
                   m_config.m_per_ip_max_tokens, m_config.m_per_ip_refill_rate,
                   m_config.m_per_ip_max_ips,
                   m_config.m_per_ip_cleanup_ttl_seconds);
    } else {
      Logger::info("Per-IP rate limiting disabled");
    }

    // Per-client metrics collector (X-DataHub-Client-Id header). Purely
    // observability: lets Grafana tell apart clients that share one IP.
    // Counters are recorded directly by the request handler.
    m_proxy.m_per_client_id_metrics_collector =
        std::make_shared<LabeledCounterCollector>(
            "client_id", "l2_proxy_per_client_id_requests_total",
            "Total number of requests received per X-DataHub-Client-Id header",
            "l2_proxy_per_client_id_rejected_total",
            "Total number of requests rejected by rate limiters per "
            "X-DataHub-Client-Id header");

    // Per-client latency histogram (same label) for p50/p95/p99 panels per
    // client in Grafana. Buckets mirror the global request-duration histogram.
    m_proxy.m_per_client_id_latency_collector =
        std::make_shared<LabeledHistogramCollector>(
            "client_id", "l2_proxy_per_client_id_latency_seconds",
            "Request processing latency per X-DataHub-Client-Id header",
            std::vector<double>(
                histogram_buckets::g_k_latency_5ms_to_10s.begin(),
                histogram_buckets::g_k_latency_5ms_to_10s.end()));

    // Per-client duplicate counter: counts duplicate POST bodies per
    // X-DataHub-Client-Id header so Grafana can show which clients repeat the
    // same payload (retry storms) instead of only an aggregate rate.
    m_proxy.m_per_client_id_duplicate_collector = std::make_shared<
        LabeledCounterCollector>(
        "client_id", "l2_proxy_per_client_id_duplicate_requests_total",
        "Total number of duplicate POST bodies (same body hash seen again) "
        "detected per X-DataHub-Client-Id header",
        "l2_proxy_per_client_id_duplicate_rejected_total",
        "Reserved: rejected duplicate POSTs per X-DataHub-Client-Id header");

    // Duplicate POST detector: keys bodies by SHA-256, keeps a bounded report
    // of the top duplicates. Served on GET /debug/duplicates.
    DuplicateDetector::Options dup_options;
    dup_options.m_enabled = m_config.m_duplicate_detection_enabled;
    dup_options.m_top_n = m_config.m_duplicate_detection_top_n;
    dup_options.m_max_entries = m_config.m_duplicate_detection_max_entries;
    dup_options.m_max_body_bytes =
        m_config.m_duplicate_detection_max_body_bytes;
    dup_options.m_ttl_ms = m_config.m_duplicate_detection_ttl_ms;
    m_proxy.m_duplicate_detector =
        std::make_unique<DuplicateDetector>(dup_options);
    Logger::info("Duplicate POST detector initialized: enabled={} top_n={} "
                 "max_entries={} max_body_bytes={} ttl_ms={}",
                 dup_options.m_enabled, dup_options.m_top_n,
                 dup_options.m_max_entries, dup_options.m_max_body_bytes,
                 dup_options.m_ttl_ms);
  }
}

AppContext::~AppContext() {
  // Stop the JaegerLogger sender thread before the prometheus registries
  // (which own the traced metrics) are destroyed in reverse member order.
  m_tracer.reset();
}

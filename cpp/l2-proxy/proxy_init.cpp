#include "proxy_init.hpp"
#include "app_context.hpp"
#include "duplicate_detector.hpp"
#include "dynamic_labeled_family.hpp"
#include "logger.hpp"
#include "metrics_manager.hpp"
#include "nats_client.hpp"
#include "rate_limiter.hpp"
#include "rate_limiter_per_ip.hpp"

void init_proxy_components(AppContext &app_ctx) {
  Logger::info("Using NATS for messaging (host={}:{}, subject={})",
               app_ctx.m_config.m_nats_host, app_ctx.m_config.m_nats_port,
               app_ctx.m_config.m_nats_subject);

  app_ctx.m_nats_client =
      std::make_shared<NatsClient>(app_ctx.m_config.create_nats_config());

  if (!app_ctx.m_nats_client->connect()) {
    Logger::error("Failed to connect to NATS server");
  } else {
    Logger::info("NATS client connected successfully");
  }

  app_ctx.m_proxy.m_internal_memory_metrics =
      std::make_unique<InternalMemoryMetrics>(
          InternalMemoryMetrics{MetricsManager::create_gauge(
              app_ctx.m_proxy_registry, "l2_proxy_per_ip_rate_limiter_ips_tracked",
              "Current number of unique IPs tracked by per-IP rate limiter")});

  app_ctx.m_proxy.m_rate_limiter_metrics =
      std::make_unique<RateLimiterMetrics>(RateLimiterMetrics{
          MetricsManager::create_gauge(app_ctx.m_proxy_registry,
                                       "l2_rate_limiter_tokens",
                                       "Available rate limiter tokens"),
          MetricsManager::create_counter(
              app_ctx.m_proxy_registry, "l2_rate_limiter_rejected_total",
              "Total requests rejected by global rate limiter")});

  app_ctx.m_proxy.m_per_ip_rate_limiter_metrics =
      std::make_unique<PerIPRateLimiterMetrics>(
          PerIPRateLimiterMetrics{MetricsManager::create_counter(
              app_ctx.m_proxy_registry, "l2_per_ip_rate_limiter_rejected_total",
              "Total requests rejected by per-IP rate limiter")});

  if (app_ctx.m_config.m_enable_global_rate_limiting) {
    app_ctx.m_proxy.m_rate_limiter = std::make_unique<RateLimiter>(
        static_cast<uint64_t>(app_ctx.m_config.m_global_max_tokens),
        static_cast<uint64_t>(app_ctx.m_config.m_global_refill_rate));
    Logger::info("Global rate limiter initialized: max={} tokens, "
                 "refill={}/sec",
                 app_ctx.m_config.m_global_max_tokens,
                 app_ctx.m_config.m_global_refill_rate);
  } else {
    Logger::info(
        "Global rate limiter disabled (ENABLE_GLOBAL_RATE_LIMITING=false)");
  }

  if (app_ctx.m_config.m_enable_per_ip_rate_limiting) {
    app_ctx.m_proxy.m_per_ip_rate_limiter = std::make_unique<PerIPRateLimiter>(
        app_ctx.m_config.m_per_ip_max_tokens,
        app_ctx.m_config.m_per_ip_refill_rate, app_ctx.m_config.m_per_ip_max_ips,
        app_ctx.m_config.m_per_ip_cleanup_ttl_seconds);
    app_ctx.m_proxy.m_per_ip_metrics_collector =
        std::make_shared<DynamicLabeledFamily<prometheus::Gauge>>(
            "ip",
            std::vector<DynamicLabeledFamily<prometheus::Gauge>::Series>{
                {"l2_proxy_per_ip_requests_total",
                 "Total number of requests received per client IP"},
                {"l2_proxy_per_ip_rejected_total",
                 "Total number of requests rejected by the per-IP rate limiter "
                 "per client IP"}},
            [limiter = app_ctx.m_proxy.m_per_ip_rate_limiter.get()]() {
              std::vector<std::pair<std::string, std::vector<double>>> entries;
              if (limiter != nullptr) {
                for (const auto &[ip, stats] : limiter->get_per_ip_stats()) {
                  entries.emplace_back(ip, std::vector<double>{
                                               static_cast<double>(
                                                   stats.m_requests),
                                               static_cast<double>(
                                                   stats.m_rejected)});
                }
              }
              return entries;
            });
    Logger::info("Per-IP rate limiter initialized: max_tokens={} "
                 "refill_rate={} max_ips={} cleanup_ttl={}s",
                 app_ctx.m_config.m_per_ip_max_tokens,
                 app_ctx.m_config.m_per_ip_refill_rate,
                 app_ctx.m_config.m_per_ip_max_ips,
                 app_ctx.m_config.m_per_ip_cleanup_ttl_seconds);
  } else {
    Logger::info("Per-IP rate limiting disabled");
  }

  app_ctx.m_proxy.m_per_client_id_metrics_collector =
      std::make_shared<DynamicLabeledFamily<prometheus::Counter>>(
          "client_id",
          std::vector<DynamicLabeledFamily<prometheus::Counter>::Series>{
              {"l2_proxy_per_client_id_requests_total",
               "Total number of requests received per X-DataHub-Client-Id "
               "header"},
              {"l2_proxy_per_client_id_rejected_total",
               "Total number of requests rejected by rate limiters per "
               "X-DataHub-Client-Id header"}});

  app_ctx.m_proxy.m_per_client_id_latency_collector =
      std::make_shared<DynamicLabeledFamily<prometheus::Histogram>>(
          "client_id",
          std::vector<DynamicLabeledFamily<prometheus::Histogram>::Series>{
              {"l2_proxy_per_client_id_latency_seconds",
               "Request processing latency per X-DataHub-Client-Id header"}},
          DynamicLabeledFamily<prometheus::Histogram>::Provider{}, 300, 10000,
          std::vector<double>(
              histogram_buckets::g_k_latency_5ms_to_10s.begin(),
              histogram_buckets::g_k_latency_5ms_to_10s.end()));

  app_ctx.m_proxy.m_per_client_id_duplicate_collector =
      std::make_shared<DynamicLabeledFamily<prometheus::Counter>>(
          "client_id",
          std::vector<DynamicLabeledFamily<prometheus::Counter>::Series>{
              {"l2_proxy_per_client_id_duplicate_requests_total",
               "Total number of duplicate POST bodies (same body hash seen "
               "again) detected per X-DataHub-Client-Id header"},
              {"l2_proxy_per_client_id_duplicate_rejected_total",
               "Reserved: rejected duplicate POSTs per X-DataHub-Client-Id "
               "header"}});

  DuplicateDetector::Options dup_options;
  dup_options.m_enabled = app_ctx.m_config.m_duplicate_detection_enabled;
  dup_options.m_top_n = app_ctx.m_config.m_duplicate_detection_top_n;
  dup_options.m_max_entries = app_ctx.m_config.m_duplicate_detection_max_entries;
  dup_options.m_max_body_bytes =
      app_ctx.m_config.m_duplicate_detection_max_body_bytes;
  dup_options.m_ttl_ms = app_ctx.m_config.m_duplicate_detection_ttl_ms;
  dup_options.m_duplicate_log_threshold =
      app_ctx.m_config.m_duplicate_log_threshold;
  dup_options.m_per_client_max_entries =
      app_ctx.m_config.m_duplicate_detection_max_clients;
  dup_options.m_per_client_ttl_ms =
      app_ctx.m_config.m_duplicate_detection_client_ttl_ms;
  app_ctx.m_proxy.m_duplicate_detector =
      std::make_unique<DuplicateDetector>(dup_options);
  Logger::info("Duplicate POST detector initialized: enabled={} top_n={} "
               "max_entries={} max_body_bytes={} ttl_ms={} log_threshold={} "
               "max_clients={} client_ttl_ms={}",
               dup_options.m_enabled, dup_options.m_top_n,
               dup_options.m_max_entries, dup_options.m_max_body_bytes,
               dup_options.m_ttl_ms, dup_options.m_duplicate_log_threshold,
               dup_options.m_per_client_max_entries,
               dup_options.m_per_client_ttl_ms);
}
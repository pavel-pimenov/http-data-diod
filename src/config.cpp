#include "config.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "nats_client.hpp"
#include "nlohmann/json.hpp"
#include "string_utils.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <sstream>

// Defined in version.cpp (kept in its own TU so version bumps recompile only
// that file, not the heavy config/main translation units).
extern const char *g_l2_proxy_version;

namespace {
// Single place for reading an env var. Returns false if not set.
bool get_env_raw(const std::string &env_name, std::string &out_value) {
  const char *env_val = std::getenv(env_name.c_str());
  if (!env_val) {
    return false;
  }
  out_value = env_val;
  return true;
}

// Logs the fallback when an env var is not set. Shared by all get_env_*
// overloads so the message stays consistent.
void log_env_default(const std::string &env_name,
                     const std::string &default_val) {
  Logger::info("Using default {}: {} (override with {} environment variable)",
               env_name, default_val, env_name);
}
} // namespace

namespace {
// Accumulates Config::validate() issues. Errors set the final result to false
// (and log when requested); warnings only log.
struct ConfigChecker {
  explicit ConfigChecker(bool log_issues) : m_log_issues(log_issues) {}
  void operator()(bool cond, const std::string &msg, bool is_error = true) {
    check(cond, msg, is_error);
  }
  void check(bool cond, const std::string &msg, bool is_error = true) {
    if (cond) {
      return;
    }
    if (is_error) {
      if (m_log_issues) {
        Logger::error("{}", msg);
      }
      m_valid = false;
    } else if (m_log_issues) {
      Logger::warn("{}", msg);
    }
  }
  [[nodiscard]] bool valid() const { return m_valid; }

private:
  bool m_log_issues;
  bool m_valid = true;
};

bool in_range(int val, int lo, int hi) { return val >= lo && val <= hi; }
bool positive(int val) { return val > 0; }
bool non_negative(int val) { return val >= 0; }
bool one_of(const std::string &val,
            std::initializer_list<std::string> opts) {
  return std::ranges::contains(opts, val);
}

void validate_ports_and_timeouts(const Config &cfg, ConfigChecker &check) {
  // Ports
  check(in_range(cfg.m_proxy.m_port, 1, 65535),
        std::format("Invalid proxy port: {} (must be 1-65535)",
                    cfg.m_proxy.m_port));
  check(in_range(cfg.m_server.m_port, 1, 65535),
        std::format("Invalid L2 server port: {} (must be 1-65535)",
                    cfg.m_server.m_port));

  // Timeouts
  check(positive(cfg.m_proxy.m_request_timeout_seconds),
        std::format("Invalid request timeout: {} (must be > 0)",
                    cfg.m_proxy.m_request_timeout_seconds));
  check(positive(cfg.m_proxy.m_http_timeout_seconds),
        std::format("Invalid HTTP timeout: {} (must be > 0)",
                    cfg.m_proxy.m_http_timeout_seconds));
}

void validate_mode_and_urls(const Config &cfg, ConfigChecker &check) {
  // Mode & log level
  check(one_of(cfg.m_app.m_mode, {"proxy", "worker", "l2-server"}),
        std::format(
            "Invalid mode: {} (must be 'proxy', 'worker', or 'l2-server')",
            cfg.m_app.m_mode));
  check(
      one_of(cfg.m_app.m_log_level, {"DEBUG", "INFO", "WARN", "ERROR"}),
      std::format(
          "Invalid log level: {} (must be 'DEBUG', 'INFO', 'WARN', or 'ERROR')",
          cfg.m_app.m_log_level));

  // URLs (only proxy/worker build a real URL; l2-server leaves them empty)
  if (cfg.m_app.m_mode != "l2-server") {
    check(!cfg.m_server.m_url.empty(), "L2 server URL cannot be empty");
    check(!cfg.m_server.m_urls.empty(), "L2 server URLs cannot be empty");
  }
}

void validate_protocols_and_ssl(const Config &cfg, ConfigChecker &check) {
  // Protocols
  check(
      one_of(cfg.m_server.m_protocol, {"http", "https"}),
      std::format("Invalid L2 server protocol: {} (must be 'http' or 'https')",
                  cfg.m_server.m_protocol));
  check(one_of(cfg.m_proxy.m_protocol, {"http", "https"}),
        std::format("Invalid proxy protocol: {} (must be 'http' or 'https')",
                    cfg.m_proxy.m_protocol));

  // SSL for proxy
  if (cfg.m_proxy.m_protocol == "https") {
    check(!cfg.m_ssl.m_server_cert_file.empty(),
          "SSL_SERVER_CERT_FILE is required when PROXY_PROTOCOL=https");
    check(!cfg.m_ssl.m_server_key_file.empty(),
          "SSL_SERVER_KEY_FILE is required when PROXY_PROTOCOL=https");
  }

  // SSL for L2 server
  if (cfg.m_server.m_protocol == "https") {
    check(!cfg.m_ssl.m_server_cert_file.empty(),
          "SSL_SERVER_CERT_FILE is required when L2_SERVER_PROTOCOL=https");
    check(!cfg.m_ssl.m_server_key_file.empty(),
          "SSL_SERVER_KEY_FILE is required when L2_SERVER_PROTOCOL=https");
  }
}

void validate_threading_and_pool(const Config &cfg, ConfigChecker &check) {
  // Server threads, timeout, pool type, worker threads, retries, HTTP pool
  check(one_of(cfg.m_app.m_thread_pool_type, {"custom", "none"}),
        std::format("Invalid thread pool type: {} (must be 'custom' or 'none')",
                    cfg.m_app.m_thread_pool_type));
  check(positive(cfg.m_worker.m_threads),
        std::format("Invalid L2 worker threads: {} (must be > 0)",
                    cfg.m_worker.m_threads));
  check(non_negative(cfg.m_worker.m_queue_size),
        std::format("Invalid L2 worker queue size: {} (must be >= 0, "
                    "0 = auto)",
                    cfg.m_worker.m_queue_size));
  check(non_negative(cfg.m_proxy.m_max_retries),
        std::format("Invalid max retries: {} (must be >= 0)",
                    cfg.m_proxy.m_max_retries));
  check(positive(cfg.m_proxy.m_http_pool_size),
        std::format("Invalid HTTP pool size: {} (must be > 0)",
                    cfg.m_proxy.m_http_pool_size));
  check(cfg.m_proxy.m_http_pool_size <= 1000,
        std::format("Very large HTTP pool size: {} (recommended: < 1000)",
                    cfg.m_proxy.m_http_pool_size),
        false);
  check(positive(cfg.m_proxy.m_http_pool_idle_timeout_seconds),
        std::format("Invalid HTTP_POOL_IDLE_TIMEOUT_SECONDS: {} (must be > 0)",
                    cfg.m_proxy.m_http_pool_idle_timeout_seconds));
}

void validate_nats_and_db_query(const Config &cfg, ConfigChecker &check) {
  // NATS (used only in proxy/worker modes)
  if (cfg.m_app.m_mode == "proxy" || cfg.m_app.m_mode == "worker") {
    check(in_range(cfg.m_nats.m_port, 1, 65535),
          std::format("Invalid NATS port: {} (must be 1-65535)",
                      cfg.m_nats.m_port));
    check(!cfg.m_nats.m_host.empty(), "NATS host cannot be empty");
    check(!cfg.m_nats.m_subject.empty(), "NATS subject cannot be empty");
    check(positive(cfg.m_nats.m_timeout_ms),
          std::format("Invalid NATS timeout: {} (must be > 0)",
                      cfg.m_nats.m_timeout_ms));

    // NATS TLS
    if (cfg.m_nats.m_enable_tls) {
      check(!cfg.m_nats.m_tls_ca_cert_file.empty(),
            "NATS_TLS_CA_CERT_FILE is required when NATS_ENABLE_TLS=true");
      check(cfg.m_nats.m_tls_cert_file.empty() == cfg.m_nats.m_tls_key_file.empty(),
            "NATS_TLS_CERT_FILE and NATS_TLS_KEY_FILE must be set together");
    }

    // DB Gateway
    if (cfg.m_db_query.m_enabled) {
      check(!cfg.m_db_query.m_subject.empty(),
            "DB_QUERY_NATS_SUBJECT cannot be empty");
      check(positive(cfg.m_db_query.m_timeout_ms),
            std::format("Invalid DB_QUERY_NATS_TIMEOUT_MS: {} (must be > 0)",
                        cfg.m_db_query.m_timeout_ms));
      check(positive(cfg.m_db_query.m_default_timeout_ms),
            std::format("Invalid DB_QUERY_DEFAULT_TIMEOUT_MS: {} (must be > 0)",
                        cfg.m_db_query.m_default_timeout_ms));
      check(positive(cfg.m_db_query.m_default_max_rows),
            std::format("Invalid DB_QUERY_DEFAULT_MAX_ROWS: {} (must be > 0)",
                        cfg.m_db_query.m_default_max_rows));
      for (const auto &db : cfg.m_db_query.m_databases) {
        check(db.m_driver == "oracle" || db.m_driver == "postgres",
              std::format("DB '{}': unknown driver '{}'", db.m_name,
                          db.m_driver));
        // Proxy mode registers DBs for routing/validation only; the connection
        // fields are checked by the worker (which owns the pools).
        if (cfg.m_app.m_mode == "proxy") {
          continue;
        }
        check(!db.m_host.empty(),
              std::format("DB '{}': host cannot be empty", db.m_name));
        check(in_range(db.m_port, 1, 65535),
              std::format("DB '{}': invalid port {} (must be 1-65535)",
                          db.m_name, db.m_port));
        if (db.m_driver == "oracle") {
          check(!db.m_service.empty(),
                std::format("DB '{}': service cannot be empty", db.m_name));
        } else {
          check(!db.m_database.empty(),
                std::format("DB '{}': database cannot be empty", db.m_name));
        }
        check(!db.m_user.empty(),
              std::format("DB '{}': user cannot be empty", db.m_name));
        check(db.m_pool_min >= 1 && db.m_pool_max >= db.m_pool_min,
              std::format("DB '{}': invalid pool (min={} max={})", db.m_name,
                          db.m_pool_min, db.m_pool_max));
      }
    }
  }
}

void validate_rate_limiting(const Config &cfg, ConfigChecker &check) {
  // Per-IP Rate Limiting
  if (cfg.m_rate_limit.m_per_ip.m_enabled) {
    check(positive(cfg.m_rate_limit.m_per_ip.m_max_tokens),
          std::format("Invalid PER_IP_MAX_TOKENS: {} (must be > 0)",
                      cfg.m_rate_limit.m_per_ip.m_max_tokens));
    check(positive(cfg.m_rate_limit.m_per_ip.m_refill_rate),
          std::format("Invalid PER_IP_REFILL_RATE: {} (must be > 0)",
                      cfg.m_rate_limit.m_per_ip.m_refill_rate));
    check(positive(cfg.m_rate_limit.m_per_ip.m_max_ips),
          std::format("Invalid PER_IP_MAX_IPS: {} (must be > 0)",
                      cfg.m_rate_limit.m_per_ip.m_max_ips));
    check(non_negative(cfg.m_rate_limit.m_per_ip.m_cleanup_ttl_seconds),
          std::format("Invalid PER_IP_CLEANUP_TTL_SECONDS: {} (must be >= 0)",
                      cfg.m_rate_limit.m_per_ip.m_cleanup_ttl_seconds));
  }

  // Global Rate Limiting
  if (cfg.m_rate_limit.m_global.m_enabled) {
    check(positive(cfg.m_rate_limit.m_global.m_max_tokens),
          std::format("Invalid GLOBAL_RATE_LIMIT_MAX_TOKENS: {} (must be > 0)",
                      cfg.m_rate_limit.m_global.m_max_tokens));
    check(positive(cfg.m_rate_limit.m_global.m_refill_rate),
          std::format("Invalid GLOBAL_RATE_LIMIT_REFILL_RATE: {} (must be > 0)",
                      cfg.m_rate_limit.m_global.m_refill_rate));
  }
}

void validate_dedup_and_duplicates(const Config &cfg, ConfigChecker &check) {
  // Dedup cache
  if (cfg.m_dedup.m_enabled) {
    check(positive(cfg.m_dedup.m_max_entries),
          std::format("Invalid DEDUP_MAX_ENTRIES: {} (must be > 0)",
                      cfg.m_dedup.m_max_entries));
    check(positive(cfg.m_dedup.m_ttl_ms),
          std::format("Invalid DEDUP_TTL_MS: {} (must be > 0)",
                      cfg.m_dedup.m_ttl_ms));
  }

  // Duplicate detection
  if (cfg.m_duplicate.m_enabled) {
    check(positive(cfg.m_duplicate.m_top_n),
          std::format("Invalid DUPLICATE_DETECTION_TOP_N: {} (must be > 0)",
                      cfg.m_duplicate.m_top_n));
    check(
        positive(cfg.m_duplicate.m_max_entries),
        std::format(
            "Invalid DUPLICATE_DETECTION_MAX_ENTRIES: {} (must be > 0)",
            cfg.m_duplicate.m_max_entries));
    check(positive(cfg.m_duplicate.m_ttl_ms),
          std::format("Invalid DUPLICATE_DETECTION_TTL_MS: {} (must be > 0)",
                      cfg.m_duplicate.m_ttl_ms));
    check(non_negative(cfg.m_duplicate.m_max_clients),
          std::format(
              "Invalid DUPLICATE_DETECTION_MAX_CLIENTS: {} (must be >= 0, "
              "0 = unbounded)",
              cfg.m_duplicate.m_max_clients));
    check(non_negative(cfg.m_duplicate.m_client_ttl_ms),
          std::format(
              "Invalid DUPLICATE_DETECTION_CLIENT_TTL_MS: {} (must be >= 0, "
              "0 = no TTL eviction)",
              cfg.m_duplicate.m_client_ttl_ms));
    check(non_negative(cfg.m_duplicate.m_max_body_bytes),
          std::format(
              "Invalid DUPLICATE_DETECTION_MAX_BODY_BYTES: {} (must be >= 0)",
              cfg.m_duplicate.m_max_body_bytes));
    check(non_negative(cfg.m_duplicate.m_log_threshold),
          std::format("Invalid DUPLICATE_LOG_THRESHOLD: {} (must be >= 0)",
                      cfg.m_duplicate.m_log_threshold));
  }
}

void validate_tracing(const Config &cfg, ConfigChecker &check) {
  // Tracing settings
  check(cfg.m_tracing.m_batch_size > 0,
        std::format("Invalid tracing batch size: {} (must be > 0)",
                    cfg.m_tracing.m_batch_size));
  check(positive(cfg.m_tracing.m_flush_interval_ms),
        std::format("Invalid tracing flush interval: {} (must be > 0)",
                    cfg.m_tracing.m_flush_interval_ms));
  check(cfg.m_tracing.m_sample_rate >= 0.0 && cfg.m_tracing.m_sample_rate <= 1.0,
        std::format("Invalid tracing sample rate: {} (must be 0.0-1.0)",
                    cfg.m_tracing.m_sample_rate));
  check(cfg.m_sentry.m_sample_rate >= 0.0 && cfg.m_sentry.m_sample_rate <= 1.0,
        std::format("Invalid sentry sample rate: {} (must be 0.0-1.0)",
                    cfg.m_sentry.m_sample_rate));
  // Tracing outage circuit-breaker: threshold must trip at least after one
  // failure, cooldown must stay within [base, max].
  check(positive(cfg.m_tracing.m_outage_failure_threshold),
        std::format("Invalid tracing outage failure threshold: {} "
                    "(must be > 0)",
                    cfg.m_tracing.m_outage_failure_threshold));
  check(positive(cfg.m_tracing.m_outage_cooldown_base_ms),
        std::format("Invalid tracing outage cooldown base: {} (must be > 0)",
                    cfg.m_tracing.m_outage_cooldown_base_ms));
  check(cfg.m_tracing.m_outage_cooldown_max_ms >=
            cfg.m_tracing.m_outage_cooldown_base_ms,
        std::format("Invalid tracing outage cooldown max: {} (must be >= "
                    "base {})",
                    cfg.m_tracing.m_outage_cooldown_max_ms,
                    cfg.m_tracing.m_outage_cooldown_base_ms));
}
} // namespace

void Config::load_from_env() {
  load_l2_server_config();
  load_server_timeout_config();
  load_feature_config();
  if (uses_nats()) {
    load_nats_config();
    load_db_query_config();
  }
  m_app.m_crash_test = get_env_bool("CRASH_TEST", m_app.m_crash_test);
  m_app.m_enable_crash_test_endpoint =
      get_env_bool("ENABLE_CRASH_TEST_ENDPOINT", m_app.m_enable_crash_test_endpoint);
  m_app.m_health_ready_allow_connect =
      get_env_bool("HEALTH_READY_ALLOW_CONNECT", m_app.m_health_ready_allow_connect);
}

void Config::load_l2_server_config() {
  m_app.m_mode = get_env_string("MODE", m_app.m_mode);
  const auto l2_server_host = get_env_string("L2_SERVER_HOST", "l2-server");
  m_server.m_port = get_env_int("L2_SERVER_PORT", m_server.m_port);
  m_server.m_protocol =
      get_env_protocol("L2_SERVER_PROTOCOL", m_server.m_protocol);

  if (m_app.m_mode == "l2-server") {
    // The l2-server binds on m_server.m_port / m_server.m_protocol but never
    // calls itself. L2_SERVER_* only tell proxy/worker how to reach this
    // service, so leave the URL fields empty instead of pointing at itself.
    m_server.m_url.clear();
    m_server.m_urls.clear();
    Logger::info(
        "Mode l2-server: L2_SERVER_* only configure how proxy/worker reach "
        "this service; URL fields left empty");
    return;
  }

  m_server.m_url = std::format("{}://{}:{}", m_server.m_protocol,
                                l2_server_host, m_server.m_port);

  const auto l2_urls_env = get_env_string("L2_SERVER_URLS", "");
  if (!l2_urls_env.empty()) {
    try {
      const auto urls_result = JsonUtils::try_parse(l2_urls_env);
      if (urls_result && JsonUtils::is_array(*urls_result)) {
        m_server.m_urls.clear();
        for (const auto &url : *urls_result) {
          if (url.is_string()) {
            m_server.m_urls.push_back(url);
          }
        }
        Logger::info("L2_SERVER_URLS loaded: {} URLs", m_server.m_urls.size());
      } else {
        Logger::warn(
            "L2_SERVER_URLS is not a valid JSON array, using fallback");
        m_server.m_urls = {m_server.m_url};
      }
    } catch (const std::exception &e) {
      Logger::warn("Failed to parse L2_SERVER_URLS: {}, using fallback",
                   e.what());
      m_server.m_urls = {m_server.m_url};
    }
  } else {
    m_server.m_urls = {m_server.m_url};
    Logger::info("L2_SERVER_URLS not set, using single URL: {}",
                 m_server.m_url);
  }
}

void Config::load_server_timeout_config() {
  m_tracing.m_url = get_env_string("JAEGER_URL", m_tracing.m_url);
  m_sentry.m_dsn = get_env_string("SENTRY_DSN", m_sentry.m_dsn);
  m_sentry.m_environment = get_env_string("SENTRY_ENVIRONMENT", m_sentry.m_environment);
  m_sentry.m_release = get_env_string("SENTRY_RELEASE", m_sentry.m_release);
  // Without an explicit SENTRY_RELEASE the running build version becomes the
  // release, so GlitchTip events are attributable to a code revision.
  if (m_sentry.m_release.empty()) {
    m_sentry.m_release = g_l2_proxy_version;
  }
  m_sentry.m_sample_rate =
      get_env_double("SENTRY_SAMPLE_RATE", m_sentry.m_sample_rate);
  m_sentry.m_timeout_ms = get_env_int("SENTRY_TIMEOUT_MS", m_sentry.m_timeout_ms);
  m_sentry.m_max_queue_size =
      get_env_int("SENTRY_MAX_QUEUE_SIZE", 256);
  if (!m_sentry.m_dsn.empty()) {
    Logger::info(
        "Sentry DSN configured: project={} release={} environment={} "
        "queue_limit={}",
        m_sentry.m_dsn.substr(m_sentry.m_dsn.find_last_of('/') + 1),
        m_sentry.m_release, m_sentry.m_environment, m_sentry.m_max_queue_size);
  }
  m_proxy.m_request_timeout_seconds =
      get_env_int("REQUEST_TIMEOUT_SECONDS", m_proxy.m_request_timeout_seconds);
  m_proxy.m_http_timeout_seconds =
      get_env_int("HTTP_TIMEOUT_SECONDS", m_proxy.m_http_timeout_seconds);
  m_server.m_test_response_delay_ms =
      get_env_int("L2_TEST_RESPONSE_DELAY_MS", m_server.m_test_response_delay_ms);
  m_tracing.m_enable = get_env_bool("ENABLE_TRACING", m_tracing.m_enable);
  m_app.m_log_level = get_env_string("LOG_LEVEL", m_app.m_log_level);
  m_proxy.m_port = get_env_int("PROXY_PORT", m_proxy.m_port);
  m_proxy.m_protocol = get_env_protocol("PROXY_PROTOCOL", m_proxy.m_protocol);
  m_app.m_thread_pool_type = get_env_string("THREAD_POOL_TYPE", m_app.m_thread_pool_type);
  m_proxy.m_http_pool_size = get_env_int("HTTP_POOL_SIZE", m_proxy.m_http_pool_size);
  m_proxy.m_http_pool_idle_timeout_seconds = get_env_int(
      "HTTP_POOL_IDLE_TIMEOUT_SECONDS", m_proxy.m_http_pool_idle_timeout_seconds);
  m_worker.m_threads = get_env_int("L2_WORKER_THREADS", m_worker.m_threads);
  m_worker.m_queue_size =
      get_env_int("L2_WORKER_QUEUE_SIZE", m_worker.m_queue_size);
  m_proxy.m_max_retries = get_env_int("MAX_RETRIES", m_proxy.m_max_retries);
  m_ssl.m_enable_server_certificate_verification =
      get_env_bool("ENABLE_SSL_SERVER_CERTIFICATE_VERIFICATION",
                   m_ssl.m_enable_server_certificate_verification);
  m_ssl.m_enable_server_hostname_verification =
      get_env_bool("ENABLE_SSL_SERVER_HOSTNAME_VERIFICATION",
                   m_ssl.m_enable_server_hostname_verification);
  m_ssl.m_ca_cert_path = get_env_string("SSL_CA_CERT_PATH", m_ssl.m_ca_cert_path);

  m_ssl.m_server_cert_file =
      get_env_string("SSL_SERVER_CERT_FILE", m_ssl.m_server_cert_file);
  m_ssl.m_server_key_file =
      get_env_string("SSL_SERVER_KEY_FILE", m_ssl.m_server_key_file);
  if (m_proxy.m_protocol == "https" || m_server.m_protocol == "https") {
    if (m_ssl.m_server_cert_file.empty() || m_ssl.m_server_key_file.empty()) {
      Logger::warn("HTTPS protocol specified but SSL_SERVER_CERT_FILE or "
                   "SSL_SERVER_KEY_FILE not set");
    } else {
      Logger::info("HTTPS server SSL configured: cert={}, key={}",
                   m_ssl.m_server_cert_file, m_ssl.m_server_key_file);
    }
  }
}

void Config::load_feature_config() {
  m_tracing.m_batch_size = get_env_int("TRACING_BATCH_SIZE", m_tracing.m_batch_size);
  m_tracing.m_flush_interval_ms =
      get_env_int("TRACING_FLUSH_INTERVAL_MS", m_tracing.m_flush_interval_ms);
  m_tracing.m_sample_rate =
      get_env_double("TRACING_SAMPLE_RATE", m_tracing.m_sample_rate);
  // Tracing outage circuit-breaker: after TRACING_OUTAGE_FAILURE_THRESHOLD
  // consecutive delivery failures a tracing sink is shed for an exponential
  // cooldown window (base, doubled per opening, clamped by the max).
  m_tracing.m_outage_failure_threshold = get_env_int(
      "TRACING_OUTAGE_FAILURE_THRESHOLD", m_tracing.m_outage_failure_threshold);
  m_tracing.m_outage_cooldown_base_ms = get_env_int(
      "TRACING_OUTAGE_COOLDOWN_BASE_MS", m_tracing.m_outage_cooldown_base_ms);
  m_tracing.m_outage_cooldown_max_ms = get_env_int(
      "TRACING_OUTAGE_COOLDOWN_MAX_MS", m_tracing.m_outage_cooldown_max_ms);
  Logger::info(
      "Tracing config: batch_size={} flush_interval={}ms sample_rate={} "
      "outage_breaker(failures={}, cooldown={}/{}/{}ms)",
      m_tracing.m_batch_size, m_tracing.m_flush_interval_ms, m_tracing.m_sample_rate,
      m_tracing.m_outage_failure_threshold, m_tracing.m_outage_cooldown_base_ms,
      m_tracing.m_outage_cooldown_base_ms, m_tracing.m_outage_cooldown_max_ms);

  m_rate_limit.m_per_ip.m_enabled =
      get_env_bool("ENABLE_PER_IP_RATE_LIMITING", m_rate_limit.m_per_ip.m_enabled);
  m_rate_limit.m_per_ip.m_max_tokens = get_env_int("PER_IP_MAX_TOKENS", m_rate_limit.m_per_ip.m_max_tokens);
  m_rate_limit.m_per_ip.m_refill_rate = get_env_int("PER_IP_REFILL_RATE", m_rate_limit.m_per_ip.m_refill_rate);
  m_rate_limit.m_per_ip.m_max_ips = get_env_int("PER_IP_MAX_IPS", m_rate_limit.m_per_ip.m_max_ips);
  m_rate_limit.m_per_ip.m_cleanup_ttl_seconds = get_env_int(
      "PER_IP_CLEANUP_TTL_SECONDS", m_rate_limit.m_per_ip.m_cleanup_ttl_seconds);
  Logger::info("Per-IP Rate Limiting: enabled={} max_tokens={} refill_rate={} "
               "max_ips={} cleanup_ttl={}s",
               m_rate_limit.m_per_ip.m_enabled, m_rate_limit.m_per_ip.m_max_tokens,
               m_rate_limit.m_per_ip.m_refill_rate, m_rate_limit.m_per_ip.m_max_ips,
               m_rate_limit.m_per_ip.m_cleanup_ttl_seconds);

  m_rate_limit.m_global.m_enabled =
      get_env_bool("ENABLE_GLOBAL_RATE_LIMITING", m_rate_limit.m_global.m_enabled);
  m_rate_limit.m_global.m_max_tokens =
      get_env_int("GLOBAL_RATE_LIMIT_MAX_TOKENS", m_rate_limit.m_global.m_max_tokens);
  m_rate_limit.m_global.m_refill_rate =
      get_env_int("GLOBAL_RATE_LIMIT_REFILL_RATE", m_rate_limit.m_global.m_refill_rate);
  Logger::info("Global Rate Limiting: enabled={} max_tokens={} refill_rate={}",
               m_rate_limit.m_global.m_enabled, m_rate_limit.m_global.m_max_tokens,
               m_rate_limit.m_global.m_refill_rate);

  m_dedup.m_enabled = get_env_bool("DEDUP_ENABLED", m_dedup.m_enabled);
  m_dedup.m_max_entries =
      get_env_int("DEDUP_MAX_ENTRIES", m_dedup.m_max_entries);
  m_dedup.m_ttl_ms = get_env_int("DEDUP_TTL_MS", m_dedup.m_ttl_ms);
  Logger::info("Dedup cache: enabled={} max_entries={} ttl_ms={}",
               m_dedup.m_enabled, m_dedup.m_max_entries, m_dedup.m_ttl_ms);

  m_duplicate.m_enabled =
      get_env_bool("DUPLICATE_DETECTION_ENABLED", m_duplicate.m_enabled);
  m_duplicate.m_reject_enabled =
      get_env_bool("DUPLICATE_REJECT_ENABLED", m_duplicate.m_reject_enabled);
  m_duplicate.m_top_n =
      get_env_int("DUPLICATE_DETECTION_TOP_N", m_duplicate.m_top_n);
  m_duplicate.m_max_entries =
      get_env_int("DUPLICATE_DETECTION_MAX_ENTRIES",
                  m_duplicate.m_max_entries);
  m_duplicate.m_max_body_bytes =
      get_env_int("DUPLICATE_DETECTION_MAX_BODY_BYTES",
                  m_duplicate.m_max_body_bytes);
  m_duplicate.m_ttl_ms =
      get_env_int("DUPLICATE_DETECTION_TTL_MS", m_duplicate.m_ttl_ms);
  m_duplicate.m_log_threshold =
      get_env_int("DUPLICATE_LOG_THRESHOLD", m_duplicate.m_log_threshold);
  m_duplicate.m_max_clients =
      get_env_int("DUPLICATE_DETECTION_MAX_CLIENTS",
                  m_duplicate.m_max_clients);
  m_duplicate.m_client_ttl_ms =
      get_env_int("DUPLICATE_DETECTION_CLIENT_TTL_MS",
                  m_duplicate.m_client_ttl_ms);
  Logger::info("Duplicate detection: enabled={} top_n={} max_entries={} "
               "max_body_bytes={} ttl_ms={} reject_enabled={} log_threshold={} "
               "max_clients={} client_ttl_ms={}",
               m_duplicate.m_enabled, m_duplicate.m_top_n,
               m_duplicate.m_max_entries,
               m_duplicate.m_max_body_bytes,
               m_duplicate.m_ttl_ms, m_duplicate.m_reject_enabled,
               m_duplicate.m_log_threshold, m_duplicate.m_max_clients,
               m_duplicate.m_client_ttl_ms);
}

void Config::load_nats_config() {
  m_nats.m_host = get_env_string("NATS_HOST", m_nats.m_host);
  m_nats.m_port = get_env_int("NATS_PORT", m_nats.m_port);
  m_nats.m_subject = get_env_string("NATS_SUBJECT", m_nats.m_subject);
  m_nats.m_queue_group = get_env_string("NATS_QUEUE_GROUP", m_nats.m_queue_group);
  m_nats.m_timeout_ms = get_env_int("NATS_TIMEOUT_MS", m_nats.m_timeout_ms);

  m_nats.m_username = get_env_string("NATS_USERNAME", m_nats.m_username);
  m_nats.m_password = get_env_string("NATS_PASSWORD", m_nats.m_password);
  m_nats.m_token = get_env_string("NATS_TOKEN", m_nats.m_token);
  m_nats.m_credentials_file =
      get_env_string("NATS_CREDENTIALS_FILE", m_nats.m_credentials_file);
  m_nats.m_enable_tls = get_env_bool("NATS_ENABLE_TLS", m_nats.m_enable_tls);
  m_nats.m_tls_cert_file =
      get_env_string("NATS_TLS_CERT_FILE", m_nats.m_tls_cert_file);
  m_nats.m_tls_key_file =
      get_env_string("NATS_TLS_KEY_FILE", m_nats.m_tls_key_file);
  m_nats.m_tls_ca_cert_file =
      get_env_string("NATS_TLS_CA_CERT_FILE", m_nats.m_tls_ca_cert_file);

  if (!m_nats.m_username.empty() || !m_nats.m_token.empty() ||
      !m_nats.m_credentials_file.empty()) {
    Logger::info("NATS authentication: enabled");
  }
  if (m_nats.m_enable_tls) {
    Logger::info("NATS TLS: enabled");
  }

  Logger::info("NATS is the only messaging backend");
  Logger::info("NATS host: {}:{}", m_nats.m_host, m_nats.m_port);
  Logger::info("NATS subject: {}", m_nats.m_subject);
  Logger::info("NATS queue group: {}", m_nats.m_queue_group);
  Logger::info("NATS timeout: {}ms", m_nats.m_timeout_ms);
}

void Config::load_db_query_config() {
  m_db_query.m_enabled = get_env_bool("DB_QUERY_ENABLED", m_db_query.m_enabled);
  m_db_query.m_subject =
      get_env_string("DB_QUERY_NATS_SUBJECT", m_db_query.m_subject);
  m_db_query.m_queue_group =
      get_env_string("DB_QUERY_NATS_QUEUE_GROUP", m_db_query.m_queue_group);
  m_db_query.m_timeout_ms =
      get_env_int("DB_QUERY_NATS_TIMEOUT_MS", m_db_query.m_timeout_ms);
  m_db_query.m_default_timeout_ms =
      get_env_int("DB_QUERY_DEFAULT_TIMEOUT_MS", m_db_query.m_default_timeout_ms);
  m_db_query.m_default_max_rows =
      get_env_int("DB_QUERY_DEFAULT_MAX_ROWS", m_db_query.m_default_max_rows);
  if (!m_db_query.m_enabled) {
    return;
  }
  const bool oracle_enabled = get_env_bool("DB_ORACLE_ENABLED", false);
  const bool postgres_enabled = get_env_bool("DB_POSTGRES_ENABLED", false);
  if (!oracle_enabled && !postgres_enabled) {
    Logger::warn("DB_QUERY_ENABLED=true but no database driver enabled "
                 "(DB_ORACLE_ENABLED/DB_POSTGRES_ENABLED are false): no "
                 "databases configured");
    return;
  }

  // Proxy mode only registers the enabled database names/drivers, which is all
  // it needs for the /v1/sql/* listing and route validation. The driver
  // connection config (host/port/credentials/pool) is owned solely by the
  // worker, so it is not read here.
  if (m_app.m_mode == "proxy") {
    auto add_routing_db = [&](const std::string &name, bool enabled) {
      if (!enabled) {
        return;
      }
      DbConfig db;
      db.m_name = name;
      db.m_driver = name;
      m_db_query.m_databases.push_back(db);
      Logger::info("DB Gateway: registered database '{}' (driver={}) "
                   "[proxy routing only]",
                   db.m_name, db.m_driver);
    };
    add_routing_db("oracle", oracle_enabled);
    add_routing_db("postgres", postgres_enabled);
    return;
  }

  // Worker mode: full connection config for building the driver pools.
  if (oracle_enabled) {
    DbConfig db;
    db.m_name = "oracle";
    db.m_driver = "oracle";
    db.m_host = get_env_string("DB_ORACLE_HOST", "oracle");
    db.m_port = get_env_int("DB_ORACLE_PORT", 1521);
    db.m_service = get_env_string("DB_ORACLE_SERVICE", "XEPDB1");
    db.m_user = get_env_string("DB_ORACLE_USER", "");
    db.m_password = get_env_string("DB_ORACLE_PASSWORD", "");
    db.m_pool_min = get_env_int("DB_ORACLE_POOL_MIN", 1);
    db.m_pool_max = get_env_int("DB_ORACLE_POOL_MAX", 5);
    db.m_query_timeout_ms = m_db_query.m_default_timeout_ms;
    db.m_max_rows = m_db_query.m_default_max_rows;
    m_db_query.m_databases.push_back(db);
    Logger::info("DB Gateway: registered database '{}' (driver={} host={}:{} "
                 "service={})",
                 db.m_name, db.m_driver, db.m_host, db.m_port, db.m_service);
  }
  if (postgres_enabled) {
    DbConfig db;
    db.m_name = "postgres";
    db.m_driver = "postgres";
    db.m_host = get_env_string("DB_POSTGRES_HOST", "postgres");
    db.m_port = get_env_int("DB_POSTGRES_PORT", 5432);
    db.m_database = get_env_string("DB_POSTGRES_DB", "postgres");
    db.m_user = get_env_string("DB_POSTGRES_USER", "");
    db.m_password = get_env_string("DB_POSTGRES_PASSWORD", "");
    db.m_pool_min = get_env_int("DB_POSTGRES_POOL_MIN", 1);
    db.m_pool_max = get_env_int("DB_POSTGRES_POOL_MAX", 5);
    db.m_query_timeout_ms = m_db_query.m_default_timeout_ms;
    db.m_max_rows = m_db_query.m_default_max_rows;
    m_db_query.m_databases.push_back(db);
    Logger::info("DB Gateway: registered database '{}' (driver={} host={}:{} "
                 "db={})",
                 db.m_name, db.m_driver, db.m_host, db.m_port, db.m_database);
  }
}

bool Config::get_env_bool(const std::string &env_name, bool default_val) {
  std::string value;
  if (!get_env_raw(env_name, value)) {
    return default_val;
  }
  value = to_lower(value);
  if (value == "true" || value == "1" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "false" || value == "0" || value == "no" || value == "off") {
    return false;
  }
  Logger::warn("Invalid {} value '{}' (must be true/false/1/0/yes/no), "
               "using default {}",
               env_name, value, default_val);
  return default_val;
}

int Config::get_env_int(const std::string &env_name, int default_val) {
  std::string value;
  if (!get_env_raw(env_name, value)) {
    log_env_default(env_name, std::to_string(default_val));
    return default_val;
  }
  try {
    auto val = std::stoi(value);
    if (val >= 0) {
      Logger::info("{} overridden: {}", env_name, val);
      return val;
    } else {
      Logger::warn("{} must be non-negative, using default {}", env_name,
                   default_val);
      return default_val;
    }
  } catch (const std::exception &e) {
    Logger::warn("Invalid {} value '{}' using default {}", env_name, value,
                 default_val);
    return default_val;
  }
}

std::string Config::get_env_string_silent(const std::string &env_name,
                                           const std::string &default_val) {
  std::string value;
  if (get_env_raw(env_name, value)) {
    return value;
  }
  return default_val;
}

std::string Config::get_env_string(const std::string &env_name,
                                   const std::string &default_val) {
  std::string value;
  if (!get_env_raw(env_name, value)) {
    log_env_default(env_name, default_val);
    return default_val;
  }
  return value;
}

std::string Config::get_env_protocol(const std::string &env_name,
                                     const std::string &default_val) {
  std::string protocol;
  if (!get_env_raw(env_name, protocol)) {
    log_env_default(env_name, default_val);
    return default_val;
  }
  if (protocol != "http" && protocol != "https") {
    Logger::warn(
        "Invalid {} value '{}' must be 'http' or 'https'. Using default: {}",
        env_name, protocol, default_val);
    return default_val;
  }
  return protocol;
}

double Config::get_env_double(const std::string &env_name, double default_val,
                              double min_val, double max_val) {
  std::string value;
  if (!get_env_raw(env_name, value)) {
    log_env_default(env_name, std::to_string(default_val));
    return default_val;
  }
  try {
    auto val = std::stod(value);
    if (val >= min_val && val <= max_val) {
      Logger::info("{} overridden: {}", env_name, val);
      return val;
    } else {
      Logger::warn("{} must be between {} and {}, using default {}", env_name,
                   min_val, max_val, default_val);
      return default_val;
    }
  } catch (const std::exception &e) {
    Logger::warn("{} parse error: {}, using default {}", env_name, e.what(),
                 default_val);
    return default_val;
  }
}

bool Config::validate(bool log_issues) const {
  ConfigChecker checker(log_issues);

  validate_ports_and_timeouts(*this, checker);
  validate_mode_and_urls(*this, checker);
  validate_protocols_and_ssl(*this, checker);
  validate_threading_and_pool(*this, checker);
  validate_nats_and_db_query(*this, checker);
  validate_rate_limiting(*this, checker);
  validate_dedup_and_duplicates(*this, checker);
  validate_tracing(*this, checker);

  return checker.valid();
}

NatsConfig Config::create_nats_config() const {
  NatsConfig cfg;
  cfg.m_host = m_nats.m_host;
  cfg.m_port = m_nats.m_port;
  cfg.m_subject = m_nats.m_subject;
  cfg.m_queue_group = m_nats.m_queue_group;
  cfg.m_timeout_ms = m_nats.m_timeout_ms;
  cfg.m_username = m_nats.m_username;
  cfg.m_password = m_nats.m_password;
  cfg.m_token = m_nats.m_token;
  cfg.m_credentials_file = m_nats.m_credentials_file;
  cfg.m_enable_tls = m_nats.m_enable_tls;
  cfg.m_tls_cert_file = m_nats.m_tls_cert_file;
  cfg.m_tls_key_file = m_nats.m_tls_key_file;
  cfg.m_tls_ca_cert_file = m_nats.m_tls_ca_cert_file;
  return cfg;
}

#include "config.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "nats_client.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <sstream>

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

Config::Config()
    : m_mode("proxy"), m_l2_server_url("http://l2-server:8088"),
      m_jaeger_url(""), m_log_level("INFO"), m_l2_server_protocol("http"),
      m_proxy_protocol("http"), m_thread_pool_type("none"),
      m_ssl_ca_cert_path(""), m_ssl_server_cert_file(""),
      m_ssl_server_key_file(""), m_nats_host("nats-server"),
      m_nats_subject("service.proxy"), m_nats_queue_group("proxy_workers"),
      m_nats_username(""), m_nats_password(""), m_nats_token(""),
      m_nats_credentials_file(""), m_nats_tls_cert_file(""),
      m_nats_tls_key_file(""), m_nats_tls_ca_cert_file(""),
      m_db_query_nats_subject("service.db.query"),
      m_db_query_nats_queue_group("db_workers"),
      m_l2_server_urls({"http://l2-server:8088"}), m_databases(),
      m_tracing_sample_rate(1.0),
      m_tracing_batch_size(50), m_request_timeout_seconds(30),
      m_http_timeout_seconds(10), m_proxy_port(8888), m_l2_server_port(8088),
      m_http_pool_size(400), m_http_pool_idle_timeout_seconds(300),
      m_l2_worker_threads(128), m_l2_worker_queue_size(0), m_max_retries(1),
      m_tracing_flush_interval_ms(1000), m_per_ip_max_tokens(100),
      m_per_ip_refill_rate(10), m_per_ip_max_ips(10000),
      m_per_ip_cleanup_ttl_seconds(300), m_global_max_tokens(10000),
      m_global_refill_rate(1000), m_dedup_max_entries(4096),
      m_dedup_ttl_ms(60000), m_duplicate_detection_top_n(100),
      m_duplicate_detection_max_entries(1000),
      m_duplicate_detection_max_body_bytes(500),
      m_duplicate_detection_ttl_ms(60000),

      m_nats_port(4222), m_nats_timeout_ms(30000), m_test_response_delay_ms(0),
      m_db_query_nats_timeout_ms(30000),
      m_db_query_default_timeout_ms(5000), m_db_query_default_max_rows(1000),
      m_enable_tracing(false),
      m_enable_ssl_server_certificate_verification(false),
      m_enable_ssl_server_hostname_verification(false),
      m_enable_per_ip_rate_limiting(true), m_enable_global_rate_limiting(true),

      m_nats_enable_tls(false), m_dedup_enabled(false),
      m_duplicate_detection_enabled(true), m_duplicate_reject_enabled(false),
      m_db_query_enabled(false),
      m_crash_test(false), m_enable_crash_test_endpoint(false),
      m_health_ready_allow_connect(false) {}

void Config::load_from_env() {
  load_l2_server_config();
  load_server_timeout_config();
  load_feature_config();
  if (uses_nats()) {
    load_nats_config();
    load_db_query_config();
  }
  m_crash_test = get_env_bool("CRASH_TEST", false);
  m_enable_crash_test_endpoint =
      get_env_bool("ENABLE_CRASH_TEST_ENDPOINT", false);
  m_health_ready_allow_connect =
      get_env_bool("HEALTH_READY_ALLOW_CONNECT", false);
}

void Config::load_l2_server_config() {
  m_mode = get_env_string("MODE", "proxy");
  const auto l2_server_host = get_env_string("L2_SERVER_HOST", "l2-server");
  m_l2_server_port = get_env_int("L2_SERVER_PORT", 8088);
  m_l2_server_protocol = get_env_protocol("L2_SERVER_PROTOCOL", "http");

  if (m_mode == "l2-server") {
    // The l2-server binds on m_l2_server_port / m_l2_server_protocol but never
    // calls itself. L2_SERVER_* only tell proxy/worker how to reach this
    // service, so leave the URL fields empty instead of pointing at itself.
    m_l2_server_url.clear();
    m_l2_server_urls.clear();
    Logger::info(
        "Mode l2-server: L2_SERVER_* only configure how proxy/worker reach "
        "this service; URL fields left empty");
    return;
  }

  m_l2_server_url = std::format("{}://{}:{}", m_l2_server_protocol,
                                l2_server_host, m_l2_server_port);

  const auto l2_urls_env = get_env_string("L2_SERVER_URLS", "");
  if (!l2_urls_env.empty()) {
    try {
      const auto urls_result = JsonUtils::try_parse(l2_urls_env);
      if (urls_result && JsonUtils::is_array(*urls_result)) {
        m_l2_server_urls.clear();
        for (const auto &url : *urls_result) {
          if (url.is_string()) {
            m_l2_server_urls.push_back(url);
          }
        }
        Logger::info("L2_SERVER_URLS loaded: {} URLs", m_l2_server_urls.size());
      } else {
        Logger::warn(
            "L2_SERVER_URLS is not a valid JSON array, using fallback");
        m_l2_server_urls = {m_l2_server_url};
      }
    } catch (const std::exception &e) {
      Logger::warn("Failed to parse L2_SERVER_URLS: {}, using fallback",
                   e.what());
      m_l2_server_urls = {m_l2_server_url};
    }
  } else {
    m_l2_server_urls = {m_l2_server_url};
    Logger::info("L2_SERVER_URLS not set, using single URL: {}",
                 m_l2_server_url);
  }
}

void Config::load_server_timeout_config() {
  m_jaeger_url = get_env_string("JAEGER_URL", "");
  m_request_timeout_seconds = get_env_int("REQUEST_TIMEOUT_SECONDS", 30);
  m_http_timeout_seconds = get_env_int("HTTP_TIMEOUT_SECONDS", 30);
  m_test_response_delay_ms = get_env_int("L2_TEST_RESPONSE_DELAY_MS", 0);
  m_enable_tracing = get_env_bool("ENABLE_TRACING", false);
  m_log_level = get_env_string("LOG_LEVEL", "INFO");
  m_proxy_port = get_env_int("PROXY_PORT", 8888);
  m_proxy_protocol = get_env_protocol("PROXY_PROTOCOL", "http");
  m_thread_pool_type = get_env_string("THREAD_POOL_TYPE", "none");
  m_http_pool_size = get_env_int("HTTP_POOL_SIZE", 400);
  m_http_pool_idle_timeout_seconds =
      get_env_int("HTTP_POOL_IDLE_TIMEOUT_SECONDS", 300);
  m_l2_worker_threads = get_env_int("L2_WORKER_THREADS", 128);
  m_l2_worker_queue_size = get_env_int("L2_WORKER_QUEUE_SIZE", 0);
  m_max_retries = get_env_int("MAX_RETRIES", 1);
  m_enable_ssl_server_certificate_verification =
      get_env_bool("ENABLE_SSL_SERVER_CERTIFICATE_VERIFICATION", false);
  m_enable_ssl_server_hostname_verification =
      get_env_bool("ENABLE_SSL_SERVER_HOSTNAME_VERIFICATION", false);
  m_ssl_ca_cert_path = get_env_string("SSL_CA_CERT_PATH", "");

  m_ssl_server_cert_file = get_env_string("SSL_SERVER_CERT_FILE", "");
  m_ssl_server_key_file = get_env_string("SSL_SERVER_KEY_FILE", "");
  if (m_proxy_protocol == "https" || m_l2_server_protocol == "https") {
    if (m_ssl_server_cert_file.empty() || m_ssl_server_key_file.empty()) {
      Logger::warn("HTTPS protocol specified but SSL_SERVER_CERT_FILE or "
                   "SSL_SERVER_KEY_FILE not set");
    } else {
      Logger::info("HTTPS server SSL configured: cert={}, key={}",
                   m_ssl_server_cert_file, m_ssl_server_key_file);
    }
  }
}

void Config::load_feature_config() {
  m_tracing_batch_size = get_env_int("TRACING_BATCH_SIZE", 50);
  m_tracing_flush_interval_ms = get_env_int("TRACING_FLUSH_INTERVAL_MS", 1000);
  m_tracing_sample_rate = get_env_double("TRACING_SAMPLE_RATE", 1.0);
  Logger::info(
      "Tracing config: batch_size={} flush_interval={}ms sample_rate={}",
      m_tracing_batch_size, m_tracing_flush_interval_ms, m_tracing_sample_rate);

  m_enable_per_ip_rate_limiting =
      get_env_bool("ENABLE_PER_IP_RATE_LIMITING", true);
  m_per_ip_max_tokens = get_env_int("PER_IP_MAX_TOKENS", 100);
  m_per_ip_refill_rate = get_env_int("PER_IP_REFILL_RATE", 10);
  m_per_ip_max_ips = get_env_int("PER_IP_MAX_IPS", 10000);
  m_per_ip_cleanup_ttl_seconds = get_env_int("PER_IP_CLEANUP_TTL_SECONDS", 300);
  Logger::info("Per-IP Rate Limiting: enabled={} max_tokens={} refill_rate={} "
               "max_ips={} cleanup_ttl={}s",
               m_enable_per_ip_rate_limiting, m_per_ip_max_tokens,
               m_per_ip_refill_rate, m_per_ip_max_ips,
               m_per_ip_cleanup_ttl_seconds);

  m_enable_global_rate_limiting =
      get_env_bool("ENABLE_GLOBAL_RATE_LIMITING", true);
  m_global_max_tokens = get_env_int("GLOBAL_RATE_LIMIT_MAX_TOKENS", 10000);
  m_global_refill_rate = get_env_int("GLOBAL_RATE_LIMIT_REFILL_RATE", 1000);
  Logger::info("Global Rate Limiting: enabled={} max_tokens={} refill_rate={}",
               m_enable_global_rate_limiting, m_global_max_tokens,
               m_global_refill_rate);

  m_dedup_enabled = get_env_bool("DEDUP_ENABLED", false);
  m_dedup_max_entries = get_env_int("DEDUP_MAX_ENTRIES", 4096);
  m_dedup_ttl_ms = get_env_int("DEDUP_TTL_MS", 60000);
  Logger::info("Dedup cache: enabled={} max_entries={} ttl_ms={}",
               m_dedup_enabled, m_dedup_max_entries, m_dedup_ttl_ms);

  m_duplicate_detection_enabled =
      get_env_bool("DUPLICATE_DETECTION_ENABLED", true);
  m_duplicate_reject_enabled = get_env_bool("DUPLICATE_REJECT_ENABLED", false);
  m_duplicate_detection_top_n = get_env_int("DUPLICATE_DETECTION_TOP_N", 100);
  m_duplicate_detection_max_entries =
      get_env_int("DUPLICATE_DETECTION_MAX_ENTRIES", 1000);
  m_duplicate_detection_max_body_bytes =
      get_env_int("DUPLICATE_DETECTION_MAX_BODY_BYTES", 500);
  m_duplicate_detection_ttl_ms =
      get_env_int("DUPLICATE_DETECTION_TTL_MS", 60000);
  Logger::info("Duplicate detection: enabled={} top_n={} max_entries={} "
               "max_body_bytes={} ttl_ms={} reject_enabled={}",
               m_duplicate_detection_enabled, m_duplicate_detection_top_n,
               m_duplicate_detection_max_entries,
               m_duplicate_detection_max_body_bytes,
               m_duplicate_detection_ttl_ms, m_duplicate_reject_enabled);
}

void Config::load_nats_config() {
  m_nats_host = get_env_string("NATS_HOST", "nats-server");
  m_nats_port = get_env_int("NATS_PORT", 4222);
  m_nats_subject = get_env_string("NATS_SUBJECT", "service.proxy");
  m_nats_queue_group = get_env_string("NATS_QUEUE_GROUP", "proxy_workers");
  m_nats_timeout_ms = get_env_int("NATS_TIMEOUT_MS", 30000);

  m_nats_username = get_env_string("NATS_USERNAME", "");
  m_nats_password = get_env_string("NATS_PASSWORD", "");
  m_nats_token = get_env_string("NATS_TOKEN", "");
  m_nats_credentials_file = get_env_string("NATS_CREDENTIALS_FILE", "");
  m_nats_enable_tls = get_env_bool("NATS_ENABLE_TLS", false);
  m_nats_tls_cert_file = get_env_string("NATS_TLS_CERT_FILE", "");
  m_nats_tls_key_file = get_env_string("NATS_TLS_KEY_FILE", "");
  m_nats_tls_ca_cert_file = get_env_string("NATS_TLS_CA_CERT_FILE", "");

  if (!m_nats_username.empty() || !m_nats_token.empty() ||
      !m_nats_credentials_file.empty()) {
    Logger::info("NATS authentication: enabled");
  }
  if (m_nats_enable_tls) {
    Logger::info("NATS TLS: enabled");
  }

  Logger::info("NATS is the only messaging backend");
  Logger::info("NATS host: {}:{}", m_nats_host, m_nats_port);
  Logger::info("NATS subject: {}", m_nats_subject);
  Logger::info("NATS queue group: {}", m_nats_queue_group);
  Logger::info("NATS timeout: {}ms", m_nats_timeout_ms);
}

void Config::load_db_query_config() {
  m_db_query_enabled = get_env_bool("DB_QUERY_ENABLED", false);
  m_db_query_nats_subject =
      get_env_string("DB_QUERY_NATS_SUBJECT", "service.db.query");
  m_db_query_nats_queue_group =
      get_env_string("DB_QUERY_NATS_QUEUE_GROUP", "db_workers");
  m_db_query_nats_timeout_ms = get_env_int("DB_QUERY_NATS_TIMEOUT_MS", 30000);
  m_db_query_default_timeout_ms =
      get_env_int("DB_QUERY_DEFAULT_TIMEOUT_MS", 5000);
  m_db_query_default_max_rows =
      get_env_int("DB_QUERY_DEFAULT_MAX_ROWS", 1000);
  if (!m_db_query_enabled) {
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
  if (m_mode == "proxy") {
    if (oracle_enabled) {
      DbConfig db;
      db.m_name = "oracle";
      db.m_driver = "oracle";
      m_databases.push_back(db);
      Logger::info("DB Gateway: registered database '{}' (driver={}) "
                   "[proxy routing only]",
                   db.m_name, db.m_driver);
    }
    if (postgres_enabled) {
      DbConfig db;
      db.m_name = "postgres";
      db.m_driver = "postgres";
      m_databases.push_back(db);
      Logger::info("DB Gateway: registered database '{}' (driver={}) "
                   "[proxy routing only]",
                   db.m_name, db.m_driver);
    }
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
    db.m_query_timeout_ms = m_db_query_default_timeout_ms;
    db.m_max_rows = m_db_query_default_max_rows;
    m_databases.push_back(db);
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
    db.m_query_timeout_ms = m_db_query_default_timeout_ms;
    db.m_max_rows = m_db_query_default_max_rows;
    m_databases.push_back(db);
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
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(::tolower(c)); });
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
  bool valid = true;

  const auto &check = [&](bool cond, const std::string &msg,
                          bool is_error = true) {
    if (!cond) {
      if (is_error) {
        if (log_issues) {
          Logger::error("{}", msg);
        }
        valid = false;
      } else if (log_issues) {
        Logger::warn("{}", msg);
      }
    }
  };
  const auto in_range = [](int val, int lo, int hi) {
    return val >= lo && val <= hi;
  };
  const auto positive = [](int val) { return val > 0; };
  const auto non_negative = [](int val) { return val >= 0; };
  const auto one_of = [](const std::string &val,
                         std::initializer_list<std::string> opts) {
    return std::ranges::contains(opts, val);
  };

  // Ports
  check(in_range(m_proxy_port, 1, 65535),
        std::format("Invalid proxy port: {} (must be 1-65535)", m_proxy_port));
  check(in_range(m_l2_server_port, 1, 65535),
        std::format("Invalid L2 server port: {} (must be 1-65535)",
                    m_l2_server_port));

  // Timeouts
  check(positive(m_request_timeout_seconds),
        std::format("Invalid request timeout: {} (must be > 0)",
                    m_request_timeout_seconds));
  check(positive(m_http_timeout_seconds),
        std::format("Invalid HTTP timeout: {} (must be > 0)",
                    m_http_timeout_seconds));

  // Mode & log level
  check(one_of(m_mode, {"proxy", "worker", "l2-server"}),
        std::format(
            "Invalid mode: {} (must be 'proxy', 'worker', or 'l2-server')",
            m_mode));
  check(
      one_of(m_log_level, {"DEBUG", "INFO", "WARN", "ERROR"}),
      std::format(
          "Invalid log level: {} (must be 'DEBUG', 'INFO', 'WARN', or 'ERROR')",
          m_log_level));

  // URLs (only proxy/worker build a real URL; l2-server leaves them empty)
  if (m_mode != "l2-server") {
    check(!m_l2_server_url.empty(), "L2 server URL cannot be empty");
    check(!m_l2_server_urls.empty(), "L2 server URLs cannot be empty");
  }

  // Protocols
  check(
      one_of(m_l2_server_protocol, {"http", "https"}),
      std::format("Invalid L2 server protocol: {} (must be 'http' or 'https')",
                  m_l2_server_protocol));
  check(one_of(m_proxy_protocol, {"http", "https"}),
        std::format("Invalid proxy protocol: {} (must be 'http' or 'https')",
                    m_proxy_protocol));

  // SSL for proxy
  if (m_proxy_protocol == "https") {
    check(!m_ssl_server_cert_file.empty(),
          "SSL_SERVER_CERT_FILE is required when PROXY_PROTOCOL=https");
    check(!m_ssl_server_key_file.empty(),
          "SSL_SERVER_KEY_FILE is required when PROXY_PROTOCOL=https");
  }

  // SSL for L2 server
  if (m_l2_server_protocol == "https") {
    check(!m_ssl_server_cert_file.empty(),
          "SSL_SERVER_CERT_FILE is required when L2_SERVER_PROTOCOL=https");
    check(!m_ssl_server_key_file.empty(),
          "SSL_SERVER_KEY_FILE is required when L2_SERVER_PROTOCOL=https");
  }

  // Server threads, timeout, pool type, worker threads, retries, HTTP pool
  check(one_of(m_thread_pool_type, {"custom", "none"}),
        std::format("Invalid thread pool type: {} (must be 'custom' or 'none')",
                    m_thread_pool_type));
  check(positive(m_l2_worker_threads),
        std::format("Invalid L2 worker threads: {} (must be > 0)",
                    m_l2_worker_threads));
  check(non_negative(m_l2_worker_queue_size),
        std::format("Invalid L2 worker queue size: {} (must be >= 0, "
                    "0 = auto)",
                    m_l2_worker_queue_size));
  check(non_negative(m_max_retries),
        std::format("Invalid max retries: {} (must be >= 0)", m_max_retries));
  check(positive(m_http_pool_size),
        std::format("Invalid HTTP pool size: {} (must be > 0)",
                    m_http_pool_size));
  check(m_http_pool_size <= 1000,
        std::format("Very large HTTP pool size: {} (recommended: < 1000)",
                    m_http_pool_size),
        false);
  check(positive(m_http_pool_idle_timeout_seconds),
        std::format("Invalid HTTP_POOL_IDLE_TIMEOUT_SECONDS: {} (must be > 0)",
                    m_http_pool_idle_timeout_seconds));

  // NATS (used only in proxy/worker modes)
  if (uses_nats()) {
    check(in_range(m_nats_port, 1, 65535),
          std::format("Invalid NATS port: {} (must be 1-65535)", m_nats_port));
    check(!m_nats_host.empty(), "NATS host cannot be empty");
    check(!m_nats_subject.empty(), "NATS subject cannot be empty");
    check(positive(m_nats_timeout_ms),
          std::format("Invalid NATS timeout: {} (must be > 0)",
                      m_nats_timeout_ms));

    // NATS TLS
    if (m_nats_enable_tls) {
      check(!m_nats_tls_ca_cert_file.empty(),
            "NATS_TLS_CA_CERT_FILE is required when NATS_ENABLE_TLS=true");
      check(m_nats_tls_cert_file.empty() == m_nats_tls_key_file.empty(),
            "NATS_TLS_CERT_FILE and NATS_TLS_KEY_FILE must be set together");
    }

    // DB Gateway
    if (m_db_query_enabled) {
      check(!m_db_query_nats_subject.empty(),
            "DB_QUERY_NATS_SUBJECT cannot be empty");
      check(positive(m_db_query_nats_timeout_ms),
            std::format("Invalid DB_QUERY_NATS_TIMEOUT_MS: {} (must be > 0)",
                        m_db_query_nats_timeout_ms));
      check(positive(m_db_query_default_timeout_ms),
            std::format("Invalid DB_QUERY_DEFAULT_TIMEOUT_MS: {} (must be > 0)",
                        m_db_query_default_timeout_ms));
      check(positive(m_db_query_default_max_rows),
            std::format("Invalid DB_QUERY_DEFAULT_MAX_ROWS: {} (must be > 0)",
                        m_db_query_default_max_rows));
      for (const auto &db : m_databases) {
        check(db.m_driver == "oracle" || db.m_driver == "postgres",
              std::format("DB '{}': unknown driver '{}'", db.m_name,
                          db.m_driver));
        // Proxy mode registers DBs for routing/validation only; the connection
        // fields are checked by the worker (which owns the pools).
        if (m_mode == "proxy") {
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

  // Per-IP Rate Limiting
  if (m_enable_per_ip_rate_limiting) {
    check(positive(m_per_ip_max_tokens),
          std::format("Invalid PER_IP_MAX_TOKENS: {} (must be > 0)",
                      m_per_ip_max_tokens));
    check(positive(m_per_ip_refill_rate),
          std::format("Invalid PER_IP_REFILL_RATE: {} (must be > 0)",
                      m_per_ip_refill_rate));
    check(positive(m_per_ip_max_ips),
          std::format("Invalid PER_IP_MAX_IPS: {} (must be > 0)",
                      m_per_ip_max_ips));
    check(non_negative(m_per_ip_cleanup_ttl_seconds),
          std::format("Invalid PER_IP_CLEANUP_TTL_SECONDS: {} (must be >= 0)",
                      m_per_ip_cleanup_ttl_seconds));
  }

  // Global Rate Limiting
  if (m_enable_global_rate_limiting) {
    check(positive(m_global_max_tokens),
          std::format("Invalid GLOBAL_RATE_LIMIT_MAX_TOKENS: {} (must be > 0)",
                      m_global_max_tokens));
    check(positive(m_global_refill_rate),
          std::format("Invalid GLOBAL_RATE_LIMIT_REFILL_RATE: {} (must be > 0)",
                      m_global_refill_rate));
  }

  // Dedup cache
  if (m_dedup_enabled) {
    check(positive(m_dedup_max_entries),
          std::format("Invalid DEDUP_MAX_ENTRIES: {} (must be > 0)",
                      m_dedup_max_entries));
    check(
        positive(m_dedup_ttl_ms),
        std::format("Invalid DEDUP_TTL_MS: {} (must be > 0)", m_dedup_ttl_ms));
  }

  // Duplicate detection
  if (m_duplicate_detection_enabled) {
    check(positive(m_duplicate_detection_top_n),
          std::format("Invalid DUPLICATE_DETECTION_TOP_N: {} (must be > 0)",
                      m_duplicate_detection_top_n));
    check(
        positive(m_duplicate_detection_max_entries),
        std::format("Invalid DUPLICATE_DETECTION_MAX_ENTRIES: {} (must be > 0)",
                    m_duplicate_detection_max_entries));
    check(positive(m_duplicate_detection_ttl_ms),
          std::format("Invalid DUPLICATE_DETECTION_TTL_MS: {} (must be > 0)",
                      m_duplicate_detection_ttl_ms));
    check(non_negative(m_duplicate_detection_max_body_bytes),
          std::format(
              "Invalid DUPLICATE_DETECTION_MAX_BODY_BYTES: {} (must be >= 0)",
              m_duplicate_detection_max_body_bytes));
  }

  // Tracing settings
  check(m_tracing_batch_size > 0,
        std::format("Invalid tracing batch size: {} (must be > 0)",
                    m_tracing_batch_size));
  check(positive(m_tracing_flush_interval_ms),
        std::format("Invalid tracing flush interval: {} (must be > 0)",
                    m_tracing_flush_interval_ms));
  check(m_tracing_sample_rate >= 0.0 && m_tracing_sample_rate <= 1.0,
        std::format("Invalid tracing sample rate: {} (must be 0.0-1.0)",
                    m_tracing_sample_rate));

  return valid;
}

NatsConfig Config::create_nats_config() const {
  NatsConfig cfg;
  cfg.m_host = m_nats_host;
  cfg.m_port = m_nats_port;
  cfg.m_subject = m_nats_subject;
  cfg.m_queue_group = m_nats_queue_group;
  cfg.m_timeout_ms = m_nats_timeout_ms;
  cfg.m_username = m_nats_username;
  cfg.m_password = m_nats_password;
  cfg.m_token = m_nats_token;
  cfg.m_credentials_file = m_nats_credentials_file;
  cfg.m_enable_tls = m_nats_enable_tls;
  cfg.m_tls_cert_file = m_nats_tls_cert_file;
  cfg.m_tls_key_file = m_nats_tls_key_file;
  cfg.m_tls_ca_cert_file = m_nats_tls_ca_cert_file;
  return cfg;
}

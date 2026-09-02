#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <stdexcept>
#include <string>
#include <vector>

struct NatsConfig;

// Connection settings of a single named database exposed by the HTTP DB
// Gateway (endpoint /v1/sql/{db}/query). Multiple databases are supported by
// the routing layer; config populates the built-in "oracle" and "postgres"
// databases from DB_ORACLE_* / DB_POSTGRES_* env vars.
struct DbConfig {
  std::string m_name;
  std::string m_driver; // "oracle" (ODPI-C) | "postgres" (libpq)
  std::string m_host;
  int m_port = 1521;
  // Oracle service name (host:port/service connect string). PostgreSQL uses
  // m_database instead.
  std::string m_service;
  // PostgreSQL database name (dbname=...). Unused by the Oracle driver.
  std::string m_database;
  std::string m_user;
  std::string m_password;
  int m_pool_min = 1;
  int m_pool_max = 5;
  int m_query_timeout_ms = 5000;
  int m_max_rows = 1000;
};

class Config {
public:
  // ========================================================================
  // Group 1: std::string fields (32 bytes each on libstdc++)
  // ========================================================================
  std::string m_mode;
  std::string m_l2_server_url;
  std::string m_jaeger_url;
  std::string m_log_level;
  std::string m_l2_server_protocol;
  std::string m_proxy_protocol;
  std::string m_thread_pool_type;
  std::string m_ssl_ca_cert_path;
  std::string m_ssl_server_cert_file;
  std::string m_ssl_server_key_file;
  std::string m_nats_host;
  std::string m_nats_subject;
  std::string m_nats_queue_group;
  std::string m_nats_username;
  std::string m_nats_password;
  std::string m_nats_token;
  std::string m_nats_credentials_file;
  std::string m_nats_tls_cert_file;
  std::string m_nats_tls_key_file;
  std::string m_nats_tls_ca_cert_file;
  std::string m_db_query_nats_subject;
  std::string m_db_query_nats_queue_group;

  // ========================================================================
  // Group 2: std::vector fields (24 bytes each on libstdc++)
  // ========================================================================
  std::vector<std::string> m_l2_server_urls;
  // Databases exposed through the HTTP DB Gateway (/v1/sql/{db}/...).
  std::vector<DbConfig> m_databases;

  // ========================================================================
  // Group 3: double (8 bytes)
  // ========================================================================
  double m_tracing_sample_rate; // Sampling rate (0.0-1.0, 1.0 = 100%)

  // ========================================================================
  // Group 4: size_t (8 bytes)
  // ========================================================================
  size_t m_tracing_batch_size; // Batch size for sending spans to Jaeger

  // ========================================================================
  // Group 5: int fields (4 bytes each) — sorted by logical group
  // ========================================================================
  int m_request_timeout_seconds;
  int m_http_timeout_seconds;
  int m_l2_worker_threads;
  int m_l2_worker_queue_size;
  int m_proxy_port;
  int m_l2_server_port;
  int m_http_pool_size;
  int m_http_pool_idle_timeout_seconds;
  int m_max_retries;
  int m_tracing_flush_interval_ms;
  int m_per_ip_max_tokens;
  int m_per_ip_refill_rate;
  int m_per_ip_max_ips;
  int m_per_ip_cleanup_ttl_seconds;
  int m_global_max_tokens;
  int m_global_refill_rate;
  int m_dedup_max_entries;
  int m_dedup_ttl_ms;
  int m_duplicate_detection_top_n;
  int m_duplicate_detection_max_entries;
  int m_duplicate_detection_max_body_bytes;
  int m_duplicate_detection_ttl_ms;
  int m_nats_port;
  int m_nats_timeout_ms;
  // DB Gateway NATS request timeout (how long the proxy waits for a worker
  // reply) in ms.
  int m_db_query_nats_timeout_ms;
  // Default statement execution timeout in ms applied to every DB query unless
  // the request overrides it.
  int m_db_query_default_timeout_ms;
  // Default row limit applied to every DB query unless the request overrides it.
  int m_db_query_default_max_rows;
  // Test-only: random response delay in ms on the l2-server (0 = disabled).
  // Used to desynchronize response order from request order for the
  // response-to-request correlation test.
  int m_test_response_delay_ms;

  // ========================================================================
  // Group 6: bool fields (1 byte each) — packed together at the end
  // ========================================================================
  bool m_enable_tracing;
  bool m_enable_ssl_server_certificate_verification;
  bool m_enable_ssl_server_hostname_verification;
  bool m_enable_per_ip_rate_limiting;
  bool m_enable_global_rate_limiting;
  bool m_nats_enable_tls;
  bool m_dedup_enabled;
  bool m_duplicate_detection_enabled;
  // Master switch of the HTTP DB Gateway (DB_QUERY_ENABLED). When false the
  // /v1/sql/** endpoints answer 404 and the worker skips the DB subscription.
  bool m_db_query_enabled;
  // When true the proxy rejects (HTTP 409) a POST whose body hash was already
  // seen within the detector TTL instead of forwarding it to the worker.
  // Off by default: only counting/logging happens (see /debug/duplicates).
  bool m_duplicate_reject_enabled;

  bool m_crash_test;
  // Gates the /crash-test HTTP endpoint (default off). Deliberately separate
  // from m_crash_test: CRASH_TEST=true crashes at startup, while this flag
  // only arms the endpoint so test-crash-handler.py can trigger it remotely.
  bool m_enable_crash_test_endpoint;
  // When false (default) /health/ready never initiates a NATS reconnect and
  // only reports the current connection state — guaranteed non-blocking so the
  // load balancer gets a fast answer. Set true to allow the legacy ping path
  // (which may attempt a blocking connect() when the connection is lost).
  bool m_health_ready_allow_connect;

  Config();
  void load_from_env();
  [[nodiscard]] bool validate(bool log_issues = true) const;
  NatsConfig create_nats_config() const;

public:
  // Env helpers are static and used by both Config loading and early
  // components (e.g. Logger::init reads LOG_FORMAT before Config exists).
  static int get_env_int(const std::string &env_name, int default_val);
  // Reads an env var without emitting logs. Safe to call from Logger::init
  // (which runs inside std::call_once and must not trigger logging).
  static std::string get_env_string_silent(const std::string &env_name,
                                           const std::string &default_val);
  static std::string get_env_string(const std::string &env_name,
                                    const std::string &default_val);
  static std::string get_env_protocol(const std::string &env_name,
                                      const std::string &default_val);
  static double get_env_double(const std::string &env_name, double default_val,
                               double min_val = 0.0, double max_val = 1.0);
  static bool get_env_bool(const std::string &env_name, bool default_val);

private:
  // Helper methods to split load_from_env into logical groups
  void load_l2_server_config();
  void load_server_timeout_config();
  void load_feature_config();
  void load_nats_config();
  void load_db_query_config();

  // True only for modes that talk to NATS (proxy/worker); l2-server does not.
  [[nodiscard]] bool uses_nats() const {
    return m_mode == "proxy" || m_mode == "worker";
  }
};

#endif // CONFIG_HPP

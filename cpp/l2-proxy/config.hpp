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
  std::string m_mode{"proxy"};
  std::string m_l2_server_url{"http://l2-server:8088"};
  std::string m_jaeger_url;
  std::string m_sentry_dsn;
  std::string m_sentry_environment;
  std::string m_sentry_release;
  std::string m_log_level{"INFO"};
  std::string m_l2_server_protocol{"http"};
  std::string m_proxy_protocol{"http"};
  std::string m_thread_pool_type{"none"};
  std::string m_ssl_ca_cert_path;
  std::string m_ssl_server_cert_file;
  std::string m_ssl_server_key_file;
  std::string m_nats_host{"nats-server"};
  std::string m_nats_subject{"service.proxy"};
  std::string m_nats_queue_group{"proxy_workers"};
  std::string m_nats_username;
  std::string m_nats_password;
  std::string m_nats_token;
  std::string m_nats_credentials_file;
  std::string m_nats_tls_cert_file;
  std::string m_nats_tls_key_file;
  std::string m_nats_tls_ca_cert_file;
  std::string m_db_query_nats_subject{"service.db.query"};
  std::string m_db_query_nats_queue_group{"db_workers"};

  // ========================================================================
  // Group 2: std::vector fields (24 bytes each on libstdc++)
  // ========================================================================
  std::vector<std::string> m_l2_server_urls{{"http://l2-server:8088"}};
  // Databases exposed through the HTTP DB Gateway (/v1/sql/{db}/...).
  std::vector<DbConfig> m_databases;

  // ========================================================================
  // Group 3: double (8 bytes)
  // ========================================================================
  double m_tracing_sample_rate{1.0}; // Sampling rate (0.0-1.0, 1.0 = 100%)

  // ========================================================================
  // Group 4: size_t (8 bytes)
  // ========================================================================
  size_t m_tracing_batch_size{50}; // Batch size for sending spans to Jaeger
  // Max Sentry events kept in the async queue before the oldest is dropped.
  size_t m_sentry_max_queue_size{256};

  // ========================================================================
  // Group 5: int fields (4 bytes each) — sorted by logical group
  // ========================================================================
  int m_request_timeout_seconds{30};
  int m_http_timeout_seconds{30};
  int m_l2_worker_threads{128};
  int m_l2_worker_queue_size{0};
  int m_proxy_port{8888};
  int m_l2_server_port{8088};
  int m_http_pool_size{400};
  int m_http_pool_idle_timeout_seconds{300};
  int m_max_retries{1};
  int m_tracing_flush_interval_ms{1000};
  int m_sentry_timeout_ms{3000};
  int m_per_ip_max_tokens{100};
  int m_per_ip_refill_rate{10};
  int m_per_ip_max_ips{10000};
  int m_per_ip_cleanup_ttl_seconds{300};
  int m_global_max_tokens{10000};
  int m_global_refill_rate{1000};
  int m_dedup_max_entries{4096};
  int m_dedup_ttl_ms{60000};
  int m_duplicate_detection_top_n{100};
  int m_duplicate_detection_max_entries{1000};
  int m_duplicate_detection_max_body_bytes{500};
  int m_duplicate_detection_ttl_ms{60000};
  int m_duplicate_log_threshold{5};
  int m_duplicate_detection_max_clients{1000};
  int m_duplicate_detection_client_ttl_ms{1800000};
  int m_nats_port{4222};
  int m_nats_timeout_ms{30000};
  // DB Gateway NATS request timeout (how long the proxy waits for a worker
  // reply) in ms.
  int m_db_query_nats_timeout_ms{30000};
  // Default statement execution timeout in ms applied to every DB query unless
  // the request overrides it.
  int m_db_query_default_timeout_ms{5000};
  // Default row limit applied to every DB query unless the request overrides it.
  int m_db_query_default_max_rows{1000};
  // Test-only: random response delay in ms on the l2-server (0 = disabled).
  // Used to desynchronize response order from request order for the
  // response-to-request correlation test.
  int m_test_response_delay_ms{0};

  // ========================================================================
  // Group 6: bool fields (1 byte each) — packed together at the end
  // ========================================================================
  bool m_enable_tracing{false};
  bool m_enable_ssl_server_certificate_verification{false};
  bool m_enable_ssl_server_hostname_verification{false};
  bool m_enable_per_ip_rate_limiting{true};
  bool m_enable_global_rate_limiting{true};
  bool m_nats_enable_tls{false};
  bool m_dedup_enabled{false};
  bool m_duplicate_detection_enabled{true};
  // Master switch of the HTTP DB Gateway (DB_QUERY_ENABLED). When false the
  // /v1/sql/** endpoints answer 404 and the worker skips the DB subscription.
  bool m_db_query_enabled{false};
  // When true the proxy rejects (HTTP 409) a POST whose body hash was already
  // seen within the detector TTL instead of forwarding it to the worker.
  // Off by default: only counting/logging happens (see /debug/duplicates).
  bool m_duplicate_reject_enabled{false};

  bool m_crash_test{false};
  // Gates the /crash-test HTTP endpoint (default off). Deliberately separate
  // from m_crash_test: CRASH_TEST=true crashes at startup, while this flag
  // only arms the endpoint so test-crash-handler.py can trigger it remotely.
  bool m_enable_crash_test_endpoint{false};
  // When false (default) /health/ready never initiates a NATS reconnect and
  // only reports the current connection state — guaranteed non-blocking so the
  // load balancer gets a fast answer. Set true to allow the legacy ping path
  // (which may attempt a blocking connect() when the connection is lost).
  bool m_health_ready_allow_connect{false};

  Config() = default;
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

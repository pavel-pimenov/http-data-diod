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
  // Nested configuration groups (each member keeps its m_ prefix, the group
  // instance is accessed as m_<group>.m_<member>).
  // ========================================================================

  // SSL/TLS tuning shared by the HTTP(S) endpoints and TLS-secured transports.
  // The server cert/key are consumed where the httplib server binds
  // (main.cpp), the CA bundle + verification toggles by the HTTP client pool,
  // and the NATS TLS files live in m_nats.
  struct Ssl {
    bool m_enable_server_certificate_verification = false;
    bool m_enable_server_hostname_verification = false;
    std::string m_ca_cert_path;
    std::string m_server_cert_file;
    std::string m_server_key_file;
  };

  // NATS messaging connection settings (the only supported backend; the redis
  // backend is disabled in this project).
  struct Nats {
    std::string m_host{"nats-server"};
    int m_port{4222};
    std::string m_subject{"service.proxy"};
    std::string m_queue_group{"proxy_workers"};
    std::string m_username;
    std::string m_password;
    std::string m_token;
    std::string m_credentials_file;
    std::string m_tls_cert_file;
    std::string m_tls_key_file;
    std::string m_tls_ca_cert_file;
    bool m_enable_tls = false;
    int m_timeout_ms{30000};
  };

  // Distributed-tracing to Jaeger (OTLP/HTTP spans) with the outage
  // circuit-breaker: after m_outage_failure_threshold consecutive delivery
  // failures a tracing sink is shed for an exponential cooldown window
  // (base, doubled per opening, clamped by the max).
  struct Tracing {
    bool m_enable = false;
    std::string m_url;
    double m_sample_rate{1.0}; // Sampling rate (0.0-1.0, 1.0 = 100%)
    size_t m_batch_size{50};   // Batch size for sending spans to Jaeger
    int m_flush_interval_ms{1000};
    int m_outage_failure_threshold{3};
    int m_outage_cooldown_base_ms{1000};
    int m_outage_cooldown_max_ms{30000};
  };

  // Sentry/GlitchTip error- and performance-transaction delivery.
  struct Sentry {
    std::string m_dsn;
    std::string m_environment;
    std::string m_release;
    double m_sample_rate{1.0}; // Sentry trace sampling 0.0-1.0 (1.0 = 100%)
    // Max Sentry events kept in the async queue before the oldest is dropped.
    size_t m_max_queue_size{256};
    int m_timeout_ms{3000};
  };

  // HTTP DB Gateway (/v1/sql/{db}/...): the worker subscribes to a NATS
  // channel, executes statements against the configured databases and replies;
  // the proxy only registers names/drivers for routing and validation.
  struct DbQuery {
    std::string m_subject{"service.db.query"};
    std::string m_queue_group{"db_workers"};
    // Databases exposed through the HTTP DB Gateway (/v1/sql/{db}/...).
    std::vector<DbConfig> m_databases;
    // Master switch (DB_QUERY_ENABLED). When false the /v1/sql/** endpoints
    // answer 404 and the worker skips the DB subscription.
    bool m_enabled = false;
    // Request timeout (how long the proxy waits for a worker reply) in ms.
    int m_timeout_ms{30000};
    // Default statement execution timeout in ms applied to every DB query
    // unless the request overrides it.
    int m_default_timeout_ms{5000};
    // Default row limit applied to every DB query unless the request
    // overrides it.
    int m_default_max_rows{1000};
  };

  // Request rate limiting: per-IP token buckets (one RateLimiter per client
  // IP, LRU-bounded) plus a global token bucket shared across all clients.
  struct RateLimit {
    struct PerIp {
      bool m_enabled = true;
      int m_max_tokens{100};
      int m_refill_rate{10};
      int m_max_ips{10000};
      int m_cleanup_ttl_seconds{300};
    } m_per_ip;
    struct Global {
      bool m_enabled = true;
      int m_max_tokens{10000};
      int m_refill_rate{1000};
    } m_global;
  };

  // Response dedup cache: caches produced responses by request_id so a
  // re-delivered NATS request is answered without a duplicate upstream call.
  struct Dedup {
    bool m_enabled = false;
    int m_max_entries{4096};
    int m_ttl_ms{60000};
  };

  // Proxy-side POST-body duplicate detection (keyed by SHA-256 hash of the
  // body). When m_reject_enabled the proxy answers HTTP 409 for a repeated
  // body instead of forwarding it; otherwise it only counts/logs duplicates.
  struct Duplicate {
    bool m_enabled = true;
    bool m_reject_enabled = false;
    int m_top_n{100};
    int m_max_entries{1000};
    int m_max_body_bytes{500};
    int m_ttl_ms{60000};
    int m_log_threshold{5};
    int m_max_clients{1000};
    int m_client_ttl_ms{1800000};
  };

  // App-wide mode/behavior switches shared by all binaries.
  struct App {
    std::string m_mode{"proxy"};
    std::string m_log_level{"INFO"};
    std::string m_thread_pool_type{"none"};
    // CRASH_TEST=true crashes the process at startup (used to exercise the
    // supervisor restart path).
    bool m_crash_test{false};
    // Gates the /crash-test HTTP endpoint (default off). Deliberately separate
    // from m_crash_test: CRASH_TEST=true crashes at startup, while this flag
    // only arms the endpoint so test-crash-handler.py can trigger it remotely.
    bool m_enable_crash_test_endpoint{false};
    // When false (default) /health/ready never initiates a NATS reconnect and
    // only reports the current connection state — guaranteed non-blocking so
    // the load balancer gets a fast answer. Set true to allow the legacy ping
    // path (which may attempt a blocking connect() when the connection is
    // lost).
    bool m_health_ready_allow_connect{false};
  };

  // HTTP reverse-proxy (proxy binary): listen config and the outbound HTTP
  // pool used to forward requests to the worker/l2-server upstream.
  struct Proxy {
    std::string m_protocol{"http"};
    int m_port{8888};
    int m_http_pool_size{400};
    int m_http_pool_idle_timeout_seconds{300};
    int m_request_timeout_seconds{30};
    int m_http_timeout_seconds{30};
    int m_max_retries{1};
  };

  // Upstream l2-server HTTP endpoint(s) that the proxy (and the worker when
  // forwarding to a remote l2-server) connects to.
  struct Server {
    std::string m_url{"http://l2-server:8088"};
    std::vector<std::string> m_urls{{"http://l2-server:8088"}};
    std::string m_protocol{"http"};
    int m_port{8088};
    // Test-only: random response delay in ms on the l2-server (0 = disabled).
    // Used to desynchronize response order from request order for the
    // response-to-request correlation test.
    int m_test_response_delay_ms{0};
  };

  // NATS worker thread pool (worker/l2-server binary).
  struct Worker {
    int m_threads{128};
    int m_queue_size{0};
  };

  Ssl m_ssl;
  Nats m_nats;
  Tracing m_tracing;
  Sentry m_sentry;
  DbQuery m_db_query;
  RateLimit m_rate_limit;
  Dedup m_dedup;
  Duplicate m_duplicate;
  App m_app;
  Proxy m_proxy;
  Server m_server;
  Worker m_worker;

  Config() = default;
  void load_from_env();
  [[nodiscard]] bool validate(bool log_issues = true) const;
  NatsConfig create_nats_config() const;

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
    return m_app.m_mode == "proxy" || m_app.m_mode == "worker";
  }
};

#endif // CONFIG_HPP

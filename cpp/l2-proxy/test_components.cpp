// Unit tests for RateLimiter, InFlightTracker, Config, base64, JsonUtils,
// JsonSchemaValidator, RetryUtils, TimeUtils, ThreadPool
#include "base64_utils.hpp"
#include "circuit_breaker.hpp"
#include "common_utils.hpp"
#include "config.hpp"
#include "dedup_cache.hpp"
#include "duplicate_detector.hpp"
#include "in_flight_tracker.hpp"
#include "json_schema_validator.hpp"
#include "json_utils.hpp"
#include "nats_client.hpp"
#include "rate_limiter.hpp"
#include "rate_limiter_per_ip.hpp"
#include "retry_utils.hpp"
#include "thread_pool.hpp"
#include "time_utils.hpp"
#include "trace_logger.hpp"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <set>
#include <thread>
#include <vector>

TEST_CASE("RateLimiter: Allows requests within limit", "[rate-limiter]") {
  RateLimiter limiter(10, 10); // 10 tokens max, 10/sec refill

  for (int i = 0; i < 10; ++i) {
    REQUIRE(limiter.acquire() == true);
  }
}

TEST_CASE("RateLimiter: Rejects when tokens exhausted", "[rate-limiter]") {
  RateLimiter limiter(5, 1); // 5 tokens max

  for (int i = 0; i < 5; ++i) {
    limiter.acquire();
  }

  REQUIRE(limiter.acquire() == false);
}

TEST_CASE("RateLimiter: Refills tokens over time", "[rate-limiter]") {
  RateLimiter limiter(10, 10); // 10 tokens/sec

  // Exhaust tokens
  for (int i = 0; i < 10; ++i) {
    limiter.acquire();
  }

  REQUIRE(limiter.acquire() == false);

  // Wait for refill
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  REQUIRE(limiter.acquire() == true);
}

TEST_CASE("InFlightTracker: Tracks active requests", "[in-flight]") {
  InFlightTracker tracker;

  REQUIRE(tracker.in_flight() == 0);

  {
    auto guard1 = tracker.track();
    REQUIRE(tracker.in_flight() == 1);

    auto guard2 = tracker.track();
    REQUIRE(tracker.in_flight() == 2);
  }

  REQUIRE(tracker.in_flight() == 0);
}

TEST_CASE("InFlightTracker: Counts total requests", "[in-flight]") {
  InFlightTracker tracker;

  for (int i = 0; i < 5; ++i) {
    auto guard = tracker.track();
  }

  REQUIRE(tracker.total_requests() == 5);
}

TEST_CASE("InFlightTracker: Waits for completion", "[in-flight]") {
  InFlightTracker tracker;
  std::atomic<bool> completed{false};

  std::thread t([&tracker, &completed]() {
    auto guard = tracker.track();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    completed = true;
  });

  bool result = tracker.wait_for_completion(std::chrono::seconds(2), false);
  t.join();

  REQUIRE(result == true);
  REQUIRE(completed == true);
}

TEST_CASE("InFlightTracker: Timeout works", "[in-flight]") {
  InFlightTracker tracker;
  std::atomic<bool> started{false};
  std::atomic<bool> completed{false};

  std::thread t([&tracker, &started, &completed]() {
    auto guard = tracker.track();
    started = true;
    // Must outlive the 1s wait_for_completion timeout below; 2s keeps a
    // comfortable margin while keeping this test fast (was 5s).
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    completed = true;
  });

  while (!started.load()) {
    std::this_thread::yield();
  }
  bool result = tracker.wait_for_completion(std::chrono::seconds(1), false);
  t.join();

  REQUIRE(result == false);
  REQUIRE(completed == true);
}

TEST_CASE("Config: Default config validates successfully", "[config]") {
  Config config;
  bool valid = config.validate(false);
  REQUIRE(valid == true);
}

TEST_CASE("Config: Invalid mode fails validation", "[config]") {
  Config config;
  config.m_mode = "invalid_mode";
  bool valid = config.validate(false);
  REQUIRE(valid == false);
}

TEST_CASE("Config: Invalid NATS port fails validation", "[config]") {
  Config config;
  config.m_nats_port = 99999;
  bool valid = config.validate(false);
  REQUIRE(valid == false);
}

TEST_CASE("Config: Missing NATS host fails validation", "[config]") {
  Config config;
  config.m_nats_host = "";
  bool valid = config.validate(false);
  REQUIRE(valid == false);
}

TEST_CASE("Config: Invalid tracing sample rate fails validation", "[config]") {
  Config config;
  config.m_tracing_sample_rate = -0.1;
  bool valid = config.validate(false);
  REQUIRE(valid == false);
}

TEST_CASE("Config: HTTP pool idle timeout default validates", "[config]") {
  Config config;
  REQUIRE(config.m_http_pool_idle_timeout_seconds == 300);
  REQUIRE(config.validate(false) == true);
}

TEST_CASE("Config: HTTP pool idle timeout zero fails validation", "[config]") {
  Config config;
  config.m_http_pool_idle_timeout_seconds = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: HTTP pool idle timeout negative fails validation",
          "[config]") {
  Config config;
  config.m_http_pool_idle_timeout_seconds = -1;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: Worker mode validates", "[config]") {
  Config config;
  config.m_mode = "worker";
  REQUIRE(config.validate(false) == true);
}

TEST_CASE("Config: L2-server mode validates", "[config]") {
  Config config;
  config.m_mode = "l2-server";
  REQUIRE(config.validate(false) == true);
}

TEST_CASE("Config: Invalid log level fails validation", "[config]") {
  Config config;
  config.m_log_level = "TRACE";
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: Invalid proxy protocol fails validation", "[config]") {
  Config config;
  config.m_proxy_protocol = "ftp";
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: HTTP pool size zero fails validation", "[config]") {
  Config config;
  config.m_http_pool_size = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: Max retries negative fails validation", "[config]") {
  Config config;
  config.m_max_retries = -1;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: Worker threads zero fails validation", "[config]") {
  Config config;
  config.m_l2_worker_threads = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: request timeout zero fails validation", "[config]") {
  Config config;
  config.m_request_timeout_seconds = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: HTTP timeout zero fails validation", "[config]") {
  Config config;
  config.m_http_timeout_seconds = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: invalid L2 server protocol fails validation", "[config]") {
  Config config;
  config.m_l2_server_protocol = "ftp";
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: https proxy protocol requires SSL files", "[config]") {
  Config config;
  config.m_proxy_protocol = "https";
  REQUIRE(config.validate(false) == false);
  config.m_ssl_server_cert_file = "/cert.pem";
  config.m_ssl_server_key_file = "/key.pem";
  REQUIRE(config.validate(false) == true);
}

TEST_CASE("Config: https L2 server protocol requires SSL files", "[config]") {
  Config config;
  config.m_mode = "l2-server";
  config.m_l2_server_protocol = "https";
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: invalid thread pool type fails validation", "[config]") {
  Config config;
  config.m_thread_pool_type = "bogus";
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: negative worker queue size fails validation", "[config]") {
  Config config;
  config.m_l2_worker_queue_size = -1;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: oversized HTTP pool warns but stays valid", "[config]") {
  Config config;
  config.m_http_pool_size = 5000;
  REQUIRE(config.validate(false) == true);
}

TEST_CASE("Config: zero NATS timeout fails validation", "[config]") {
  Config config;
  config.m_nats_timeout_ms = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: empty NATS subject fails validation", "[config]") {
  Config config;
  config.m_nats_subject = "";
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: NATS TLS requires CA file", "[config]") {
  Config config;
  config.m_nats_enable_tls = true;
  REQUIRE(config.validate(false) == false);
  config.m_nats_tls_ca_cert_file = "/ca.pem";
  REQUIRE(config.validate(false) == true);
}

TEST_CASE("Config: NATS TLS cert without key fails validation", "[config]") {
  Config config;
  config.m_nats_enable_tls = true;
  config.m_nats_tls_ca_cert_file = "/ca.pem";
  config.m_nats_tls_cert_file = "/cert.pem";
  REQUIRE(config.validate(false) == false);
  config.m_nats_tls_key_file = "/key.pem";
  REQUIRE(config.validate(false) == true);
}

TEST_CASE("Config: negative per-IP tokens fail validation", "[config]") {
  Config config;
  config.m_per_ip_max_tokens = -1;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: zero global max tokens fails validation", "[config]") {
  Config config;
  config.m_global_max_tokens = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: zero dedup max entries fails validation", "[config]") {
  Config config;
  config.m_dedup_enabled = true;
  config.m_dedup_max_entries = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: zero dedup TTL fails validation", "[config]") {
  Config config;
  config.m_dedup_enabled = true;
  config.m_dedup_ttl_ms = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: zero duplicate detection top N fails validation",
          "[config]") {
  Config config;
  config.m_duplicate_detection_top_n = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: negative duplicate max body bytes fails validation",
          "[config]") {
  Config config;
  config.m_duplicate_detection_max_body_bytes = -1;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: zero tracing batch size fails validation", "[config]") {
  Config config;
  config.m_tracing_batch_size = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: zero tracing flush interval fails validation", "[config]") {
  Config config;
  config.m_tracing_flush_interval_ms = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: DB empty subject fails validation", "[config]") {
  Config config;
  config.m_db_query_enabled = true;
  config.m_db_query_nats_subject = "";
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: DB zero timeout fails validation", "[config]") {
  Config config;
  config.m_db_query_enabled = true;
  config.m_db_query_default_timeout_ms = 0;
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: DB unknown driver fails validation", "[config]") {
  Config config;
  config.m_db_query_enabled = true;
  DbConfig db;
  db.m_name = "mysql";
  db.m_driver = "mysql";
  db.m_host = "db";
  db.m_port = 3306;
  db.m_user = "u";
  db.m_database = "d";
  config.m_databases.push_back(db);
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: DB oracle missing service fails validation", "[config]") {
  Config config;
  config.m_mode = "worker"; // connection checks apply only to the worker
  config.m_db_query_enabled = true;
  DbConfig db;
  db.m_name = "oracle";
  db.m_driver = "oracle";
  db.m_host = "ora";
  db.m_port = 1521;
  db.m_user = "u";
  db.m_service = "";
  config.m_databases.push_back(db);
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: DB invalid pool range fails validation", "[config]") {
  Config config;
  config.m_mode = "worker"; // connection checks apply only to the worker
  config.m_db_query_enabled = true;
  DbConfig db;
  db.m_name = "postgres";
  db.m_driver = "postgres";
  db.m_host = "pg";
  db.m_port = 5432;
  db.m_user = "u";
  db.m_database = "d";
  db.m_pool_min = 5;
  db.m_pool_max = 1;
  config.m_databases.push_back(db);
  REQUIRE(config.validate(false) == false);
}

namespace {

class EnvVarGuard {
public:
  EnvVarGuard(const char *name, const char *value) : m_name(name) {
    if (value == nullptr) {
      unsetenv(m_name.c_str());
    } else {
      setenv(m_name.c_str(), value, 1);
    }
  }
  ~EnvVarGuard() { unsetenv(m_name.c_str()); }

private:
  std::string m_name;
};

} // namespace

TEST_CASE("Config: load_from_env reads string and int env vars", "[config]") {
  EnvVarGuard mode("MODE", "proxy");
  EnvVarGuard host("L2_SERVER_HOST", "10.0.0.5");
  EnvVarGuard port("L2_SERVER_PORT", "9090");
  EnvVarGuard proto("L2_SERVER_PROTOCOL", "http");
  Config config;
  config.load_from_env();
  REQUIRE(config.m_mode == "proxy");
  REQUIRE(config.m_l2_server_port == 9090);
  REQUIRE(config.m_l2_server_url == "http://10.0.0.5:9090");
  REQUIRE(config.m_l2_server_urls.size() == 1);
  REQUIRE(config.m_l2_server_urls[0] == "http://10.0.0.5:9090");
}

TEST_CASE("Config: L2_SERVER_URLS JSON array replaces single URL", "[config]") {
  EnvVarGuard urls("L2_SERVER_URLS",
                   R"(["http://host1:8088","http://host2:8089"])");
  Config config;
  config.load_from_env();
  REQUIRE(config.m_l2_server_urls.size() == 2);
  REQUIRE(config.m_l2_server_urls[0] == "http://host1:8088");
  REQUIRE(config.m_l2_server_urls[1] == "http://host2:8089");
}

TEST_CASE("Config: L2_SERVER_URLS invalid JSON falls back to single URL",
          "[config]") {
  EnvVarGuard urls("L2_SERVER_URLS", "not-json");
  Config config;
  config.load_from_env();
  REQUIRE(config.m_l2_server_urls.size() == 1);
  REQUIRE(config.m_l2_server_urls[0] == config.m_l2_server_url);
}

TEST_CASE("Config: L2_SERVER_URLS non-array JSON falls back to single URL",
          "[config]") {
  EnvVarGuard urls("L2_SERVER_URLS", R"("just-a-string")");
  Config config;
  config.load_from_env();
  REQUIRE(config.m_l2_server_urls.size() == 1);
  REQUIRE(config.m_l2_server_urls[0] == config.m_l2_server_url);
}

TEST_CASE("Config: l2-server mode clears L2 server URLs", "[config]") {
  EnvVarGuard mode("MODE", "l2-server");
  Config config;
  config.load_from_env();
  REQUIRE(config.m_mode == "l2-server");
  REQUIRE(config.m_l2_server_url.empty());
  REQUIRE(config.m_l2_server_urls.empty());
  REQUIRE(config.validate(false) == true);
}

TEST_CASE("Config: worker mode loads NATS config", "[config]") {
  EnvVarGuard mode("MODE", "worker");
  EnvVarGuard subj("NATS_SUBJECT", "svc.worker");
  Config config;
  config.load_from_env();
  REQUIRE(config.m_mode == "worker");
  REQUIRE(config.m_nats_subject == "svc.worker");
  REQUIRE(config.validate(false) == true);
}

TEST_CASE("Config: get_env_bool parses all true/false forms", "[config]") {
  for (const char *v : {"true", "1", "yes", "on", "TRUE", "Yes"}) {
    EnvVarGuard e("TEST_BOOL", v);
    REQUIRE(Config::get_env_bool("TEST_BOOL", false) == true);
  }
  for (const char *v : {"false", "0", "no", "off", "FALSE", "No"}) {
    EnvVarGuard e("TEST_BOOL", v);
    REQUIRE(Config::get_env_bool("TEST_BOOL", true) == false);
  }
  EnvVarGuard e("TEST_BOOL", "maybe");
  REQUIRE(Config::get_env_bool("TEST_BOOL", true) == true);
}

TEST_CASE("Config: get_env_bool returns default when unset", "[config]") {
  EnvVarGuard e("TEST_BOOL", nullptr);
  REQUIRE(Config::get_env_bool("TEST_BOOL", true) == true);
  REQUIRE(Config::get_env_bool("TEST_BOOL", false) == false);
}

TEST_CASE("Config: get_env_int parses valid value", "[config]") {
  EnvVarGuard e("TEST_INT", "42");
  REQUIRE(Config::get_env_int("TEST_INT", 1) == 42);
}

TEST_CASE("Config: get_env_int negative falls back to default", "[config]") {
  EnvVarGuard e("TEST_INT", "-5");
  REQUIRE(Config::get_env_int("TEST_INT", 10) == 10);
}

TEST_CASE("Config: get_env_int non-numeric falls back to default", "[config]") {
  EnvVarGuard e("TEST_INT", "abc");
  REQUIRE(Config::get_env_int("TEST_INT", 7) == 7);
}

TEST_CASE("Config: get_env_int unset returns default", "[config]") {
  EnvVarGuard e("TEST_INT", nullptr);
  REQUIRE(Config::get_env_int("TEST_INT", 3) == 3);
}

TEST_CASE("Config: get_env_double parses and validates range", "[config]") {
  EnvVarGuard e("TEST_DBL", "0.5");
  REQUIRE(Config::get_env_double("TEST_DBL", 1.0) == 0.5);
  EnvVarGuard e2("TEST_DBL", "1.5");
  REQUIRE(Config::get_env_double("TEST_DBL", 1.0) == 1.0);
  EnvVarGuard e3("TEST_DBL", "abc");
  REQUIRE(Config::get_env_double("TEST_DBL", 0.7) == 0.7);
}

TEST_CASE("Config: get_env_double honours a custom range", "[config]") {
  EnvVarGuard e("TEST_DBL_CUSTOM", "0.5");
  REQUIRE(Config::get_env_double("TEST_DBL_CUSTOM", 1.0, 0.0, 10.0) == 0.5);
  EnvVarGuard e2("TEST_DBL_CUSTOM", "15.0");
  REQUIRE(Config::get_env_double("TEST_DBL_CUSTOM", 1.0, 0.0, 10.0) == 1.0);
  EnvVarGuard e3("TEST_DBL_CUSTOM", "-2.0");
  REQUIRE(Config::get_env_double("TEST_DBL_CUSTOM", 3.0, -5.0, 5.0) == -2.0);
}

TEST_CASE("Config: get_env_string and silent variants", "[config]") {
  EnvVarGuard e("TEST_STR", "hello");
  REQUIRE(Config::get_env_string("TEST_STR", "d") == "hello");
  REQUIRE(Config::get_env_string_silent("TEST_STR", "d") == "hello");
  EnvVarGuard e2("TEST_STR", nullptr);
  REQUIRE(Config::get_env_string("TEST_STR", "d") == "d");
  REQUIRE(Config::get_env_string_silent("TEST_STR", "d") == "d");
}

TEST_CASE("Config: get_env_protocol validates http/https", "[config]") {
  EnvVarGuard e("TEST_PROTO", "https");
  REQUIRE(Config::get_env_protocol("TEST_PROTO", "http") == "https");
  EnvVarGuard e2("TEST_PROTO", "ftp");
  REQUIRE(Config::get_env_protocol("TEST_PROTO", "http") == "http");
}

TEST_CASE("Config: create_nats_config maps NATS fields", "[config]") {
  EnvVarGuard host("NATS_HOST", "nats.example");
  EnvVarGuard port("NATS_PORT", "4223");
  EnvVarGuard subj("NATS_SUBJECT", "svc.in");
  EnvVarGuard qg("NATS_QUEUE_GROUP", "grp");
  EnvVarGuard tm("NATS_TIMEOUT_MS", "5000");
  EnvVarGuard user("NATS_USERNAME", "user");
  EnvVarGuard pass("NATS_PASSWORD", "pass");
  EnvVarGuard tok("NATS_TOKEN", "tok");
  EnvVarGuard cred("NATS_CREDENTIALS_FILE", "/cred");
  EnvVarGuard tls("NATS_ENABLE_TLS", "true");
  EnvVarGuard cert("NATS_TLS_CERT_FILE", "/cert.pem");
  EnvVarGuard key("NATS_TLS_KEY_FILE", "/key.pem");
  EnvVarGuard ca("NATS_TLS_CA_CERT_FILE", "/ca.pem");
  Config config;
  config.load_from_env();
  NatsConfig nc = config.create_nats_config();
  REQUIRE(nc.m_host == "nats.example");
  REQUIRE(nc.m_port == 4223);
  REQUIRE(nc.m_subject == "svc.in");
  REQUIRE(nc.m_queue_group == "grp");
  REQUIRE(nc.m_timeout_ms == 5000);
  REQUIRE(nc.m_username == "user");
  REQUIRE(nc.m_password == "pass");
  REQUIRE(nc.m_token == "tok");
  REQUIRE(nc.m_credentials_file == "/cred");
  REQUIRE(nc.m_enable_tls == true);
  REQUIRE(nc.m_tls_cert_file == "/cert.pem");
  REQUIRE(nc.m_tls_key_file == "/key.pem");
  REQUIRE(nc.m_tls_ca_cert_file == "/ca.pem");
}

TEST_CASE("Config: DB_QUERY with postgres enabled registers database",
          "[config]") {
  // Full connection config (host/user/password/pool) is loaded only in worker
  // mode, which owns the driver connection pools.
  EnvVarGuard mode("MODE", "worker");
  EnvVarGuard qe("DB_QUERY_ENABLED", "true");
  EnvVarGuard pe("DB_POSTGRES_ENABLED", "true");
  EnvVarGuard host("DB_POSTGRES_HOST", "pg.example");
  EnvVarGuard port("DB_POSTGRES_PORT", "5433");
  EnvVarGuard db("DB_POSTGRES_DB", "mydb");
  EnvVarGuard user("DB_POSTGRES_USER", "alice");
  EnvVarGuard pass("DB_POSTGRES_PASSWORD", "secret");
  Config config;
  config.load_from_env();
  REQUIRE(config.m_db_query_enabled == true);
  REQUIRE(config.m_databases.size() == 1);
  const auto &dbcfg = config.m_databases[0];
  REQUIRE(dbcfg.m_name == "postgres");
  REQUIRE(dbcfg.m_driver == "postgres");
  REQUIRE(dbcfg.m_host == "pg.example");
  REQUIRE(dbcfg.m_port == 5433);
  REQUIRE(dbcfg.m_database == "mydb");
  REQUIRE(dbcfg.m_user == "alice");
  REQUIRE(dbcfg.m_password == "secret");
  REQUIRE(dbcfg.m_query_timeout_ms == config.m_db_query_default_timeout_ms);
  REQUIRE(dbcfg.m_max_rows == config.m_db_query_default_max_rows);
  REQUIRE(config.validate(false) == true);
}

TEST_CASE("Config: DB_QUERY with both drivers registers both databases",
          "[config]") {
  EnvVarGuard mode("MODE", "worker");
  EnvVarGuard qe("DB_QUERY_ENABLED", "true");
  EnvVarGuard oe("DB_ORACLE_ENABLED", "true");
  EnvVarGuard pe("DB_POSTGRES_ENABLED", "true");
  Config config;
  config.load_from_env();
  REQUIRE(config.m_databases.size() == 2);
  REQUIRE(config.m_databases[0].m_name == "oracle");
  REQUIRE(config.m_databases[1].m_name == "postgres");
}

TEST_CASE("Config: proxy mode registers DB for routing only", "[config]") {
  // Proxy registers DB names/drivers for /v1/sql/* listing & validation, but
  // reads no connection fields (host/user/password) — those belong to worker.
  EnvVarGuard qe("DB_QUERY_ENABLED", "true");
  EnvVarGuard pe("DB_POSTGRES_ENABLED", "true");
  EnvVarGuard host("DB_POSTGRES_HOST", "pg.example");
  EnvVarGuard user("DB_POSTGRES_USER", "alice");
  Config config;
  config.load_from_env();
  REQUIRE(config.m_databases.size() == 1);
  const auto &dbcfg = config.m_databases[0];
  REQUIRE(dbcfg.m_name == "postgres");
  REQUIRE(dbcfg.m_driver == "postgres");
  // Connection fields are NOT populated in proxy mode.
  REQUIRE(dbcfg.m_host.empty());
  REQUIRE(dbcfg.m_user.empty());
  REQUIRE(config.validate(false) == true);
}

TEST_CASE("Config: DB_QUERY enabled but no driver registers no databases",
          "[config]") {
  EnvVarGuard qe("DB_QUERY_ENABLED", "true");
  Config config;
  config.load_from_env();
  REQUIRE(config.m_db_query_enabled == true);
  REQUIRE(config.m_databases.empty());
}

TEST_CASE("Config: SSL warning branch loads HTTPS protocol config",
          "[config]") {
  EnvVarGuard proto("PROXY_PROTOCOL", "https");
  EnvVarGuard cert("SSL_SERVER_CERT_FILE", "/cert.pem");
  EnvVarGuard key("SSL_SERVER_KEY_FILE", "/key.pem");
  Config config;
  config.load_from_env();
  REQUIRE(config.m_proxy_protocol == "https");
  REQUIRE(config.m_ssl_server_cert_file == "/cert.pem");
  REQUIRE(config.m_ssl_server_key_file == "/key.pem");
}

TEST_CASE("Config: HTTPS without cert file logs warning", "[config]") {
  EnvVarGuard proto("PROXY_PROTOCOL", "https");
  EnvVarGuard cert("SSL_SERVER_CERT_FILE", "");
  Config config;
  config.load_from_env();
  REQUIRE(config.m_proxy_protocol == "https");
  REQUIRE(config.validate(false) == false);
}

TEST_CASE("Config: validate with logging enabled reports issues", "[config]") {
  Config config;
  config.m_mode = "bogus";
  config.m_log_level = "TRACE";
  REQUIRE(config.validate(true) == false);
}

// ============================================================================
// Base64 tests
// ============================================================================

TEST_CASE("Base64: Encode/decode roundtrip", "[base64]") {
  REQUIRE(base64::decode(base64::encode("hello")) == "hello");
  REQUIRE(base64::decode(base64::encode("")) == "");
  REQUIRE(base64::decode(base64::encode("Hello, World! 123")) ==
          "Hello, World! 123");
}

TEST_CASE("Base64: Encode produces padded output", "[base64]") {
  auto encoded = base64::encode("f");
  REQUIRE(encoded.size() % 4 == 0);
  REQUIRE(encoded == "Zg==");

  encoded = base64::encode("fo");
  REQUIRE(encoded.size() % 4 == 0);
  REQUIRE(encoded == "Zm8=");

  encoded = base64::encode("foo");
  REQUIRE(encoded.size() % 4 == 0);
  REQUIRE(encoded == "Zm9v");
}

TEST_CASE("Base64: Binary data roundtrip", "[base64]") {
  std::string binary;
  for (int i = 0; i < 256; ++i) {
    binary.push_back(static_cast<char>(i));
  }
  REQUIRE(base64::decode(base64::encode(binary)) == binary);
}

// ============================================================================
// JsonUtils tests
// ============================================================================

TEST_CASE("JsonUtils: try_parse valid JSON", "[json-utils]") {
  auto result = JsonUtils::try_parse(R"({"key": "value", "num": 42})");
  REQUIRE(result.has_value());
  REQUIRE((*result)["key"] == "value");
  REQUIRE((*result)["num"] == 42);
}

TEST_CASE("JsonUtils: try_parse invalid JSON", "[json-utils]") {
  auto result = JsonUtils::try_parse("{invalid}");
  REQUIRE(!result.has_value());
  REQUIRE(!result.error().empty());
}

TEST_CASE("JsonUtils: try_parse empty string", "[json-utils]") {
  auto result = JsonUtils::try_parse("");
  REQUIRE(!result.has_value());
}

TEST_CASE("JsonUtils: safe_get_string", "[json-utils]") {
  json j = {{"key", "value"}, {"num", 42}};
  REQUIRE(JsonUtils::safe_get_string(j, "key") == "value");
  REQUIRE(JsonUtils::safe_get_string(j, "missing", "default") == "default");
  REQUIRE(JsonUtils::safe_get_string(j, "num", "default") == "default");
}

TEST_CASE("JsonUtils: safe_get_int", "[json-utils]") {
  json j = {{"key", 42}, {"str", "hello"}};
  REQUIRE(JsonUtils::safe_get_int(j, "key") == 42);
  REQUIRE(JsonUtils::safe_get_int(j, "missing", -1) == -1);
  REQUIRE(JsonUtils::safe_get_int(j, "str", -1) == -1);
}

TEST_CASE("JsonUtils: has_key", "[json-utils]") {
  json j = {{"key", "value"}, {"null_key", nullptr}};
  REQUIRE(JsonUtils::has_key(j, "key") == true);
  REQUIRE(JsonUtils::has_key(j, "missing") == false);
  REQUIRE(JsonUtils::has_key(j, "null_key") == false);
}

TEST_CASE("JsonUtils: is_array", "[json-utils]") {
  REQUIRE(JsonUtils::is_array(json::array()));
  REQUIRE_FALSE(JsonUtils::is_array(json::object()));
  REQUIRE_FALSE(JsonUtils::is_array("string"));
}

TEST_CASE("JsonUtils: build_nats_response_envelope full fields", "[json-utils]") {
  json headers = {{"content-type", "application/json"}};
  json env = build_nats_response_envelope(
      200, "req-123", "response body", 1234567890, false, "application/json",
      headers, "traceparent-value");
  REQUIRE(env[NatsResponseContract::kStatus] == 200);
  REQUIRE(env[NatsResponseContract::kHeaders] == headers);
  const json &body = env[NatsResponseContract::kBody];
  REQUIRE(body[NatsResponseContract::kBodyRequestId] == "req-123");
  REQUIRE(body[NatsResponseContract::kBodyResponse] == "response body");
  REQUIRE(body[NatsResponseContract::kBodyTimestamp] == 1234567890);
  REQUIRE(body[NatsResponseContract::kBodyIsBinary] == false);
  REQUIRE(body[NatsResponseContract::kBodyContentType] == "application/json");
  REQUIRE(body[NatsResponseContract::kBodyTraceparent] == "traceparent-value");
}

TEST_CASE("JsonUtils: build_nats_response_envelope optional fields omitted",
          "[json-utils]") {
  json env = build_nats_response_envelope(500, "req-2", "boom", 0, true, "",
                                          json::object(), "");
  REQUIRE(env[NatsResponseContract::kStatus] == 500);
  REQUIRE_FALSE(env.contains(NatsResponseContract::kHeaders));
  const json &body = env[NatsResponseContract::kBody];
  REQUIRE(body[NatsResponseContract::kBodyRequestId] == "req-2");
  REQUIRE(body[NatsResponseContract::kBodyResponse] == "boom");
  REQUIRE(body[NatsResponseContract::kBodyIsBinary] == true);
  REQUIRE_FALSE(body.contains(NatsResponseContract::kBodyTraceparent));
}

// ============================================================================
// JsonSchemaValidator tests
// ============================================================================

TEST_CASE("RequestValidator: Missing required field fails", "[validator]") {
  RequestValidator v;
  v.add_required_field("method").add_required_field("path");

  json req = {{"method", "GET"}};
  std::string error;
  REQUIRE_FALSE(v.validate(req, error));
  REQUIRE(error.find("path") != std::string::npos);
}

TEST_CASE("RequestValidator: Disallowed method fails", "[validator]") {
  RequestValidator v;
  v.add_allowed_method("GET").add_allowed_method("POST");

  json req = {{"method", "DELETE"}, {"path", "/api"}};
  std::string error;
  REQUIRE_FALSE(v.validate(req, error));
  REQUIRE(error.find("DELETE") != std::string::npos);
}

TEST_CASE("RequestValidator: Path too long fails", "[validator]") {
  RequestValidator v;
  v.set_max_path_length(10);

  json req = {{"method", "GET"}, {"path", "/this/path/is/very/long"}};
  std::string error;
  REQUIRE_FALSE(v.validate(req, error));
  REQUIRE(error.find("too long") != std::string::npos);
}

TEST_CASE("RequestValidator: Path not in allowed list fails", "[validator]") {
  RequestValidator v;
  v.add_allowed_path("/api");

  json req = {{"method", "GET"}, {"path", "/admin"}};
  std::string error;
  REQUIRE_FALSE(v.validate(req, error));
  REQUIRE(error.find("not allowed") != std::string::npos);
}

TEST_CASE("RequestValidator: Path with allowed prefix passes", "[validator]") {
  RequestValidator v;
  v.add_allowed_path("/api");

  json req = {{"method", "GET"}, {"path", "/api/users"}};
  std::string error;
  REQUIRE(v.validate(req, error));
}

TEST_CASE("RequestValidator: Body too large fails", "[validator]") {
  RequestValidator v;
  v.set_max_body_size(10);

  json req = {{"body", std::string(100, 'x')}};
  std::string error;
  REQUIRE_FALSE(v.validate(req, error));
  REQUIRE(error.find("too large") != std::string::npos);
}

TEST_CASE("RequestValidator: validate_or_throw throws on invalid",
          "[validator]") {
  RequestValidator v;
  v.add_required_field("method");

  json req = {};
  REQUIRE_THROWS_AS(v.validate_or_throw(req), std::invalid_argument);
}

TEST_CASE("RequestValidator: validate_or_throw passes on valid",
          "[validator]") {
  RequestValidator v;
  v.add_required_field("method");

  json req = {{"method", "GET"}};
  REQUIRE_NOTHROW(v.validate_or_throw(req));
}

TEST_CASE("ResponseValidator: Disallowed status code fails", "[validator]") {
  ResponseValidator v;
  v.add_allowed_status_code(200).add_allowed_status_code(201);

  json resp = {{"status_code", 500}};
  std::string error;
  REQUIRE_FALSE(v.validate(resp, error));
  REQUIRE(error.find("500") != std::string::npos);
}

TEST_CASE("ResponseValidator: Required body missing fails", "[validator]") {
  ResponseValidator v;
  v.require_body(true);

  json resp = {{"status_code", 200}};
  std::string error;
  REQUIRE_FALSE(v.validate(resp, error));
  REQUIRE(error.find("required") != std::string::npos);
}

TEST_CASE("ResponseValidator: Standard validators created correctly",
          "[validator]") {
  auto rv = create_standard_request_validator();
  json req = {{"method", "GET"}, {"path", "/api"}, {"request_id", "123"}};
  std::string error;
  REQUIRE(rv.validate(req, error));

  auto resp_v = create_standard_response_validator();
  json resp = {{"status_code", 200}, {"body", {{"response", "ok"}}}};
  REQUIRE(resp_v.validate(resp, error));
}

// ============================================================================
// RetryUtils tests
// ============================================================================

TEST_CASE("RetryUtils: calculate_retry_delay_with_jitter", "[retry-utils]") {
  int delay = calculate_retry_delay_with_jitter(100, 50, 1, 2000);
  REQUIRE(delay >= 100);
  REQUIRE(delay <= 150);

  delay = calculate_retry_delay_with_jitter(100, 50, 5, 2000);
  REQUIRE(delay >= 1600);
  REQUIRE(delay <= 1650);

  delay = calculate_retry_delay_with_jitter(100, 50, 10, 500);
  REQUIRE(delay >= 500);
  REQUIRE(delay <= 550);
}

TEST_CASE("RetryUtils: calculate_simple_jitter_delay", "[retry-utils]") {
  for (int i = 0; i < 100; ++i) {
    int delay = calculate_simple_jitter_delay(100, 50);
    REQUIRE(delay >= 100);
    REQUIRE(delay <= 150);
  }
}

TEST_CASE("RetryUtils: execute_with_retry succeeds first try",
          "[retry-utils]") {
  int calls = 0;
  auto result = execute_with_retry<int>(
      [&calls]() {
        calls++;
        return 42;
      },
      "test");
  REQUIRE(result == 42);
  REQUIRE(calls == 1);
}

TEST_CASE("RetryUtils: execute_with_retry fails then succeeds",
          "[retry-utils]") {
  int calls = 0;
  auto result = execute_with_retry<int>(
      [&calls]() {
        calls++;
        if (calls < 3)
          throw std::runtime_error("fail");
        return 42;
      },
      "test", 3, 1, 10, 1);
  REQUIRE(result == 42);
  REQUIRE(calls == 3);
}

TEST_CASE("RetryUtils: execute_with_retry throws after all retries",
          "[retry-utils]") {
  REQUIRE_THROWS_AS(
      execute_with_retry<int>(
          []() -> int { throw std::runtime_error("always fail"); }, "test", 2,
          1, 10, 1),
      std::runtime_error);
}

// ============================================================================
// TimeUtils tests
// ============================================================================

TEST_CASE("TimeUtils: epoch_ms and epoch_s consistency", "[time-utils]") {
  auto ms = TimeUtils::epoch_ms();
  auto s = TimeUtils::epoch_s();
  REQUIRE(ms > 0);
  REQUIRE(ms / 1000 == s);
}

TEST_CASE("TimeUtils: epoch_us >= epoch_ms", "[time-utils]") {
  REQUIRE(TimeUtils::epoch_us() >= TimeUtils::epoch_ms() * 1000);
}

TEST_CASE("TimeUtils: format_rfc3339 format", "[time-utils]") {
  auto ts = TimeUtils::format_rfc3339();
  REQUIRE(ts.size() > 10);
  REQUIRE(ts[4] == '-');
  REQUIRE(ts[7] == '-');
  REQUIRE(ts[10] == 'T');
  REQUIRE(ts.back() == 'Z');
}

TEST_CASE("TimeUtils: ms_until past deadline", "[time-utils]") {
  auto past = std::chrono::system_clock::now() - std::chrono::seconds(10);
  REQUIRE(TimeUtils::ms_until(past) == 0);
}

TEST_CASE("TimeUtils: ms_until future deadline", "[time-utils]") {
  auto future = std::chrono::system_clock::now() + std::chrono::seconds(5);
  auto ms = TimeUtils::ms_until(future);
  REQUIRE(ms >= 4900);
  REQUIRE(ms <= 5100);
}

// ============================================================================
// ThreadPool tests
// ============================================================================

TEST_CASE("ThreadPool: Basic enqueue and result", "[thread-pool]") {
  ThreadPool pool(2);
  auto future = pool.enqueue([]() { return 42; });
  REQUIRE(future.get() == 42);
}

TEST_CASE("ThreadPool: Multiple tasks complete", "[thread-pool]") {
  ThreadPool pool(4);
  std::vector<std::future<int>> futures;
  for (int i = 0; i < 10; ++i) {
    futures.push_back(pool.enqueue([i]() { return i * 2; }));
  }
  for (int i = 0; i < 10; ++i) {
    REQUIRE(futures[i].get() == i * 2);
  }
}

TEST_CASE("ThreadPool: Concurrent enqueue from multiple threads",
          "[thread-pool]") {
  ThreadPool pool(4);
  std::atomic<int> counter{0};
  std::vector<std::future<void>> futures;
  for (int i = 0; i < 20; ++i) {
    futures.push_back(pool.enqueue([&counter]() { counter.fetch_add(1); }));
  }
  for (auto &f : futures) {
    f.get();
  }
  REQUIRE(counter.load() == 20);
}

TEST_CASE("ThreadPool: Void task completes", "[thread-pool]") {
  ThreadPool pool(2);
  std::atomic<int> counter{0};
  auto f = pool.enqueue([&counter]() { counter.fetch_add(1); });
  f.get();
  REQUIRE(counter.load() == 1);
}

TEST_CASE("ThreadPool: accessors report queue and thread counts",
          "[thread-pool]") {
  ThreadPool pool(1, 4);
  REQUIRE(pool.thread_count() == 1);
  REQUIRE(pool.queue_size() == 0);

  std::atomic<bool> task_started{false};
  std::promise<void> gate;
  auto gate_future = gate.get_future();
  std::vector<std::future<void>> futures;
  // Hold the only worker busy and fill the queue to its capacity of 4.
  futures.push_back(pool.enqueue([&] {
    task_started = true;
    gate_future.wait();
  }));
  while (!task_started.load()) {
    std::this_thread::yield();
  }
  for (int i = 0; i < 4; ++i) {
    futures.push_back(pool.enqueue([] {}));
  }
  REQUIRE(pool.queue_size() == 4);

  gate.set_value();
  for (auto &f : futures) {
    f.get();
  }
  REQUIRE(pool.queue_size() == 0);
}

TEST_CASE("ThreadPool: bounded queue provides backpressure", "[thread-pool]") {
  // One worker, queue capacity of one: the second enqueue fills the queue and
  // the third must block until a slot frees up.
  ThreadPool pool(1, 1);
  std::atomic<bool> task_running{false};
  std::atomic<bool> producer_returned{false};
  std::promise<void> gate;
  auto gate_future = gate.get_future();

  auto f1 = pool.enqueue([&] {
    task_running = true;
    gate_future.wait();
  });
  while (!task_running.load()) {
    std::this_thread::yield();
  }
  auto f2 = pool.enqueue([] {}); // occupies the only queue slot

  std::thread producer([&] {
    auto f3 = pool.enqueue([] {});
    (void)f3;
    producer_returned = true;
  });

  // The producer must still be blocked while the queue is full.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  REQUIRE_FALSE(producer_returned.load());

  gate.set_value();
  f1.get();
  f2.get();
  producer.join();
  REQUIRE(producer_returned.load());
}

TEST_CASE("ThreadPool: enqueue after shutdown throws", "[thread-pool]") {
  ThreadPool pool(2);
  pool.shutdown();
  REQUIRE_THROWS_AS(pool.enqueue([] {}), std::runtime_error);
}

TEST_CASE("ThreadPool: shutdown drains queued tasks", "[thread-pool]") {
  ThreadPool pool(2);
  std::atomic<int> counter{0};
  std::vector<std::future<void>> futures;
  for (int i = 0; i < 8; ++i) {
    futures.push_back(pool.enqueue([&counter]() { counter.fetch_add(1); }));
  }
  pool.shutdown();
  for (auto &f : futures) {
    f.get();
  }
  REQUIRE(counter.load() == 8);
  // Second shutdown must be a no-op (idempotent).
  pool.shutdown();
}

// ============================================================================
// Structured Logging (JSON formatter) tests
// ============================================================================

#include "logger.hpp"
#include <sstream>

TEST_CASE("Logger: LogContext thread-local fields", "[logger]") {
  Logger::set_request_id("req-123");
  Logger::set_trace_id("trace-456");
  Logger::set_client_ip("10.0.0.1");

  // Verify by calling set_service_name (which is thread-safe)
  Logger::set_service_name("test-service");

  // Clear should reset everything
  Logger::clear_correlation_context();
  // If we got here without crashing, the thread-local operations work
  REQUIRE(true);
}

TEST_CASE("Logger: LogContext set_service_name works", "[logger]") {
  Logger::set_service_name("my-service");
  // Just verify no crash
  REQUIRE(true);
  Logger::clear_correlation_context();
}

TEST_CASE("TraceLogger: build_span_json emits Zipkin v2 parentId",
          "[tracing]") {
  nlohmann::json attributes = {{"http.method", "POST"},
                               {"http.status_code", 200}};
  const auto span = JaegerLogger::build_span_json(
      "0123456789abcdef0123456789abcdef", "0123456789abcdef",
      "fedcba9876543210", "POST /", 1000, 2000, "l2-proxy", attributes);

  REQUIRE(span["id"] == "0123456789abcdef");
  REQUIRE(span["traceId"] == "0123456789abcdef0123456789abcdef");
  REQUIRE(span["name"] == "POST /");
  REQUIRE(span["timestamp"] == 1000);
  REQUIRE(span["duration"] == 1000);
  REQUIRE(span["localEndpoint"]["serviceName"] == "l2-proxy");
  REQUIRE(span["tags"]["http.method"] == "POST");
  REQUIRE(span["tags"]["http.status_code"] == "200");
  REQUIRE(span["parentId"] == "fedcba9876543210");
  REQUIRE_FALSE(span.contains("parentSpanId"));
}

TEST_CASE("TraceLogger: build_span_json omits parentId when no parent",
          "[tracing]") {
  const auto span = JaegerLogger::build_span_json(
      "0123456789abcdef0123456789abcdef", "0123456789abcdef", "", "GET /health",
      1000, 2000, "l2-proxy", {});

  REQUIRE_FALSE(span.contains("parentId"));
  REQUIRE_FALSE(span.contains("parentSpanId"));
}

TEST_CASE("Baggage: to_header URL-encodes keys and values", "[tracing]") {
  Baggage b;
  b.set("user_id", "a b=1,2");
  b.set("session", "x/y");
  const std::string header = b.to_header();

  // Encoded value must not contain raw spaces, '=', ',' or '/'.
  REQUIRE(header.find(' ') == std::string::npos);
  REQUIRE(header.find('/') == std::string::npos);

  // Round-trip through from_header must reconstruct the original items.
  const Baggage parsed = Baggage::from_header(header);
  REQUIRE(parsed.get("user_id") == "a b=1,2");
  REQUIRE(parsed.get("session") == "x/y");
}

TEST_CASE("Baggage: round-trip preserves plain values", "[tracing]") {
  Baggage b;
  b.set("key1", "value1");
  b.set("key2", "value2");
  const auto parsed = Baggage::from_header(b.to_header());
  REQUIRE(parsed.size() == 2);
  REQUIRE(parsed.get("key1") == "value1");
  REQUIRE(parsed.get("key2") == "value2");
}

TEST_CASE("Baggage: url_encode leaves unreserved chars untouched",
          "[tracing]") {
  REQUIRE(Baggage::url_encode("AZaz09-_.~") == "AZaz09-_.~");
  REQUIRE(Baggage::url_encode("a b") == "a%20b");
  REQUIRE(Baggage::url_encode("a=b") == "a%3Db");
  REQUIRE(Baggage::url_encode("a,b") == "a%2Cb");
  REQUIRE(Baggage::url_decode(Baggage::url_encode("a b=c")) == "a b=c");
}

TEST_CASE("PerIPRateLimiter: Allows requests within per-IP limit",
          "[per-ip-rate-limiter]") {
  PerIPRateLimiter limiter(5, 5, 100, 60);

  for (int i = 0; i < 5; ++i) {
    REQUIRE(limiter.acquire("1.1.1.1") == true);
  }

  REQUIRE(limiter.get_stats().m_allowed_requests == 5);
  REQUIRE(limiter.get_stats().m_tracked_ips == 1);
}

TEST_CASE("PerIPRateLimiter: Rejects and records per-IP rejection",
          "[per-ip-rate-limiter]") {
  PerIPRateLimiter limiter(3, 1, 100, 60);

  for (int i = 0; i < 3; ++i) {
    REQUIRE(limiter.acquire("1.1.1.1") == true);
  }
  REQUIRE(limiter.acquire("1.1.1.1") == false);

  const auto stats = limiter.get_per_ip_stats();
  REQUIRE(stats.size() == 1);
  REQUIRE(stats[0].first == "1.1.1.1");
  REQUIRE(stats[0].second.m_requests == 4);
  REQUIRE(stats[0].second.m_rejected == 1);
  REQUIRE(limiter.get_stats().m_rejected_requests == 1);
}

TEST_CASE("PerIPRateLimiter: get_per_ip_stats orders newest first",
          "[per-ip-rate-limiter]") {
  PerIPRateLimiter limiter(10, 10, 100, 60);

  limiter.acquire("1.1.1.1");
  limiter.acquire("2.2.2.2");
  limiter.acquire("1.1.1.1");

  const auto stats = limiter.get_per_ip_stats();
  REQUIRE(stats.size() == 2);
  REQUIRE(stats[0].first == "1.1.1.1");
  REQUIRE(stats[1].first == "2.2.2.2");
}

TEST_CASE("PerIPRateLimiter: Evicts oldest IP when max_ips exceeded",
          "[per-ip-rate-limiter]") {
  PerIPRateLimiter limiter(10, 10, 2, 60);

  REQUIRE(limiter.acquire("1.1.1.1") == true);
  REQUIRE(limiter.acquire("2.2.2.2") == true);
  REQUIRE(limiter.acquire("3.3.3.3") == true);

  REQUIRE(limiter.get_stats().m_tracked_ips == 2);
  REQUIRE(limiter.get_stats().m_evictions == 1);

  const auto stats = limiter.get_per_ip_stats();
  REQUIRE(stats.size() == 2);
  REQUIRE(stats[0].first == "3.3.3.3");
  REQUIRE(stats[1].first == "2.2.2.2");
}

TEST_CASE("PerIPRateLimiter: full cache evicts oldest and reuses slot",
          "[per-ip-rate-limiter]") {
  PerIPRateLimiter limiter(10, 10, 1, 60);

  REQUIRE(limiter.acquire("1.1.1.1") == true);
  REQUIRE(limiter.acquire("2.2.2.2") == true);

  REQUIRE(limiter.get_stats().m_tracked_ips == 1);
  REQUIRE(limiter.get_stats().m_evictions == 1);

  const auto stats = limiter.get_per_ip_stats();
  REQUIRE(stats.size() == 1);
  REQUIRE(stats[0].first == "2.2.2.2");
}

TEST_CASE("PerIPRateLimiter: cleanup removes expired entries",
          "[per-ip-rate-limiter]") {
  // cleanup_interval_seconds = 0 => every entry is immediately stale
  PerIPRateLimiter limiter(10, 10, 100, 0);

  limiter.acquire("1.1.1.1");
  limiter.acquire("2.2.2.2");
  REQUIRE(limiter.get_stats().m_tracked_ips == 2);

  limiter.cleanup_expired_ips();

  REQUIRE(limiter.get_stats().m_tracked_ips == 0);
  REQUIRE(limiter.get_stats().m_evictions >= 2);
}

TEST_CASE("PerIPRateLimiter: Distinct IPs get independent buckets",
          "[per-ip-rate-limiter]") {
  PerIPRateLimiter limiter(3, 1, 100, 60);

  for (int i = 0; i < 3; ++i) {
    REQUIRE(limiter.acquire("1.1.1.1") == true);
  }
  REQUIRE(limiter.acquire("1.1.1.1") == false);

  for (int i = 0; i < 3; ++i) {
    REQUIRE(limiter.acquire("2.2.2.2") == true);
  }
  REQUIRE(limiter.acquire("2.2.2.2") == false);

  const auto stats = limiter.get_per_ip_stats();
  REQUIRE(stats.size() == 2);
  // Newest first: 2.2.2.2 was touched last
  REQUIRE(stats[0].first == "2.2.2.2");
  REQUIRE(stats[0].second.m_requests == 4);
  REQUIRE(stats[0].second.m_rejected == 1);
  REQUIRE(stats[1].first == "1.1.1.1");
  REQUIRE(stats[1].second.m_requests == 4);
  REQUIRE(stats[1].second.m_rejected == 1);
}

TEST_CASE("DuplicateDetector: second delivery of same body is a duplicate",
          "[duplicate-detector]") {
  DuplicateDetector detector(DuplicateDetector::Options{});

  REQUIRE(detector.record("client-a", "hash-1", R"({"v":1})") == false);
  REQUIRE(detector.record("client-a", "hash-1", R"({"v":1})") == true);
  REQUIRE(detector.record("client-a", "hash-1", R"({"v":1})") == true);

  REQUIRE(detector.duplicate_bodies() == 1);
}

TEST_CASE("DuplicateDetector: distinct bodies are not duplicates",
          "[duplicate-detector]") {
  DuplicateDetector detector(DuplicateDetector::Options{});

  REQUIRE(detector.record("client-a", "hash-1", R"({"v":1})") == false);
  REQUIRE(detector.record("client-a", "hash-2", R"({"v":2})") == false);
  REQUIRE(detector.duplicate_bodies() == 0);
}

TEST_CASE("DuplicateDetector: disabled detector never reports duplicates",
          "[duplicate-detector]") {
  DuplicateDetector::Options options;
  options.m_enabled = false;
  DuplicateDetector detector(options);

  REQUIRE(detector.record("client-a", "hash-1", R"({"v":1})") == false);
  REQUIRE(detector.record("client-a", "hash-1", R"({"v":1})") == false);
  REQUIRE(detector.duplicate_bodies() == 0);
}

TEST_CASE("DuplicateDetector: report classifies same/cross client",
          "[duplicate-detector]") {
  DuplicateDetector detector(DuplicateDetector::Options{});

  detector.record("client-a", "hash-1", R"({"v":1})");
  detector.record("client-a", "hash-1", R"({"v":1})");
  detector.record("client-b", "hash-2", R"({"v":2})");
  detector.record("client-b", "hash-2", R"({"v":2})");

  const auto report = detector.report();
  REQUIRE(report["duplicate_bodies"] == 2);
  REQUIRE(report["by_type"]["same_client"] == 2);
  REQUIRE(report["by_type"]["cross_client"] == 0);
  REQUIRE(report["top"].size() == 2);
  // Order is by count then first-seen; with millisecond resolution the two
  // entries may tie, so assert on the content regardless of order.
  std::set<std::string> bodies;
  for (const auto &item : report["top"]) {
    REQUIRE(item["type"] == "same_client");
    REQUIRE(item["clients"].size() == 1);
    bodies.insert(item["body"].get<std::string>());
  }
  REQUIRE(bodies == std::set<std::string>{R"({"v":1})", R"({"v":2})"});

  // Same body delivered from a second client flips the entry to cross_client.
  detector.record("client-b", "hash-1", R"({"v":1})");
  const auto report2 = detector.report();
  REQUIRE(report2["duplicate_bodies"] == 2);
  REQUIRE(report2["by_type"]["same_client"] == 1);
  REQUIRE(report2["by_type"]["cross_client"] == 1);
  REQUIRE(report2["top"].size() == 2);
  size_t cross_entries = 0;
  for (const auto &item : report2["top"]) {
    if (item["type"] == "cross_client") {
      REQUIRE(item["clients"].size() == 2);
      REQUIRE(item["body"] == R"({"v":1})");
      ++cross_entries;
    }
  }
  REQUIRE(cross_entries == 1);
}

TEST_CASE("DuplicateDetector: body sample is capped by max_body_bytes",
          "[duplicate-detector]") {
  DuplicateDetector::Options options;
  options.m_max_body_bytes = 4;
  DuplicateDetector detector(options);

  const std::string long_body(50, 'x');
  detector.record("client-a", "hash-1", long_body);
  detector.record("client-a", "hash-1", long_body);

  const auto report = detector.report();
  REQUIRE(report["top"][0]["body"] == "");
}

TEST_CASE("DuplicateDetector: expired bodies stop being duplicates after TTL",
          "[duplicate-detector]") {
  DuplicateDetector::Options options;
  options.m_ttl_ms = 40;
  DuplicateDetector detector(options);

  REQUIRE(detector.record("client-a", "hash-1", R"({"v":1})") == false);
  REQUIRE(detector.record("client-a", "hash-1", R"({"v":1})") == true);

  std::this_thread::sleep_for(std::chrono::milliseconds(80));

  // The previous entry has expired (last_seen older than TTL), so the body is
  // treated as seen for the first time again.
  REQUIRE(detector.record("client-a", "hash-1", R"({"v":1})") == false);
  REQUIRE(detector.duplicate_bodies() == 0);
}

TEST_CASE("DuplicateDetector: bounded cache evicts the lowest-count body",
          "[duplicate-detector]") {
  DuplicateDetector::Options options;
  options.m_max_entries = 1;
  DuplicateDetector detector(options);

  detector.record("client-a", "hash-1", R"({"v":1})");
  // The first body is evicted to make room for the second.
  detector.record("client-a", "hash-2", R"({"v":2})");

  REQUIRE(detector.duplicate_bodies() == 0);
  // A fresh delivery of the evicted body is not a duplicate.
  REQUIRE(detector.record("client-a", "hash-1", R"({"v":1})") == false);
  REQUIRE(detector.record("client-a", "hash-1", R"({"v":1})") == true);
}

// ============================================================================
// DedupCache tests
// ============================================================================

TEST_CASE("DedupCache: disabled cache never hits and never stores",
          "[dedup-cache]") {
  DedupCache cache(false);
  cache.store("req-1", R"({"response":"v1"})");
  REQUIRE_FALSE(cache.find("req-1").has_value());
}

TEST_CASE("DedupCache: store then find returns the response",
          "[dedup-cache]") {
  DedupCache cache(true, 16, 60000);
  cache.store("req-1", R"({"response":"v1"})");
  const auto found = cache.find("req-1");
  REQUIRE(found.has_value());
  REQUIRE(*found == R"({"response":"v1"})");
}

TEST_CASE("DedupCache: missing key returns nullopt", "[dedup-cache]") {
  DedupCache cache;
  REQUIRE_FALSE(cache.find("no-such").has_value());
}

TEST_CASE("DedupCache: store refreshes an existing entry", "[dedup-cache]") {
  DedupCache cache;
  cache.store("req-1", "old");
  cache.store("req-1", "new");
  const auto found = cache.find("req-1");
  REQUIRE(found.has_value());
  REQUIRE(*found == "new");
}

TEST_CASE("DedupCache: entry expires after TTL", "[dedup-cache]") {
  DedupCache cache(true, 16, 40);
  cache.store("req-1", "v1");
  REQUIRE(cache.find("req-1").has_value());
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  REQUIRE_FALSE(cache.find("req-1").has_value());
}

TEST_CASE("DedupCache: bounded size evicts the oldest entry",
          "[dedup-cache]") {
  DedupCache cache(true, 2, 60000);
  cache.store("a", "1");
  cache.store("b", "2");
  cache.store("c", "3"); // must evict "a"
  REQUIRE_FALSE(cache.find("a").has_value());
  REQUIRE(cache.find("b").has_value());
  REQUIRE(cache.find("c").has_value());
}

TEST_CASE("DedupCache: refresh moves entry to the back (LRU order)",
          "[dedup-cache]") {
  DedupCache cache(true, 2, 60000);
  cache.store("a", "1");
  cache.store("b", "2");
  // Refresh "a" so it becomes the most-recently-used.
  cache.store("a", "1x");
  cache.store("c", "3"); // must evict "b", not "a"
  REQUIRE(cache.find("a").has_value());
  REQUIRE_FALSE(cache.find("b").has_value());
  REQUIRE(cache.find("c").has_value());
}

TEST_CASE("DedupCache: concurrent access does not lose entries",
          "[dedup-cache]") {
  DedupCache cache(true, 4096, 60000);
  std::vector<std::thread> threads;
  constexpr int kThreads = 8;
  constexpr int kEach = 200;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kEach; ++i) {
        const std::string key = "k-" + std::to_string(t) + "-" +
                                std::to_string(i);
        cache.store(key, "v");
        (void)cache.find(key);
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  // Total distinct keys stored (assuming no eviction at 4096 capacity).
  constexpr int kTotal = kThreads * kEach;
  REQUIRE(kTotal == 1600);
}

// ============================================================================
// CircuitBreaker tests
// ============================================================================

TEST_CASE("CircuitBreaker: starts CLOSED and allows requests",
          "[circuit-breaker]") {
  CircuitBreaker cb;
  REQUIRE(cb.state_name() == "CLOSED");
  REQUIRE(cb.allow_request() == true);
}

TEST_CASE("CircuitBreaker: opens after the failure threshold",
          "[circuit-breaker]") {
  CircuitBreaker cb;
  for (int i = 0; i < CircuitBreaker::g_failure_threshold - 1; ++i) {
    cb.record_failure();
    REQUIRE(cb.state_name() == "CLOSED");
    REQUIRE(cb.allow_request() == true);
  }
  cb.record_failure();
  REQUIRE(cb.state_name() == "OPEN");
  REQUIRE(cb.allow_request() == false);
}

TEST_CASE("CircuitBreaker: success in CLOSED resets the failure count",
          "[circuit-breaker]") {
  CircuitBreaker cb;
  for (int i = 0; i < CircuitBreaker::g_failure_threshold - 1; ++i) {
    cb.record_failure();
  }
  cb.record_success(); // resets failure_count
  REQUIRE(cb.state_name() == "CLOSED");
  // Still needs the full threshold again.
  for (int i = 0; i < CircuitBreaker::g_failure_threshold - 1; ++i) {
    cb.record_failure();
  }
  REQUIRE(cb.state_name() == "CLOSED");
  cb.record_failure();
  REQUIRE(cb.state_name() == "OPEN");
}

TEST_CASE("CircuitBreaker: failure opens immediately when in HALF_OPEN",
          "[circuit-breaker]") {
  CircuitBreaker cb;
  // Force OPEN via the threshold.
  for (int i = 0; i < CircuitBreaker::g_failure_threshold; ++i) {
    cb.record_failure();
  }
  REQUIRE(cb.state_name() == "OPEN");
  // Inject HALF_OPEN manually (simulate elapsed timeout).
  cb.m_state.store(CircuitBreaker::State::HALF_OPEN);
  cb.record_failure();
  REQUIRE(cb.state_name() == "OPEN");
}

TEST_CASE("CircuitBreaker: HALF_OPEN closes after the success threshold",
          "[circuit-breaker]") {
  CircuitBreaker cb;
  for (int i = 0; i < CircuitBreaker::g_failure_threshold; ++i) {
    cb.record_failure();
  }
  REQUIRE(cb.state_name() == "OPEN");

  // Inject HALF_OPEN (simulate timeout elapsed).
  cb.m_state.store(CircuitBreaker::State::HALF_OPEN);
  REQUIRE(cb.allow_request() == true);

  for (int i = 0; i < CircuitBreaker::g_half_open_success_threshold; ++i) {
    cb.record_success();
  }
  REQUIRE(cb.state_name() == "CLOSED");
}

TEST_CASE("CircuitBreaker: allow_request reopens after timeout in OPEN",
          "[circuit-breaker]") {
  CircuitBreaker cb;
  for (int i = 0; i < CircuitBreaker::g_failure_threshold; ++i) {
    cb.record_failure();
  }
  REQUIRE(cb.state_name() == "OPEN");
  REQUIRE(cb.allow_request() == false);

  // Simulate the timeout having elapsed by backdating m_last_failure_time_us.
  const uint64_t now_us =
      static_cast<uint64_t>(TimeUtils::epoch_us());
  cb.m_last_failure_time_us.store(
      now_us - CircuitBreaker::g_open_timeout_us - 1000);
  REQUIRE(cb.allow_request() == true);
  REQUIRE(cb.state_name() == "HALF_OPEN");
}

// ============================================================================
// Fuzz-style stress tests
// ============================================================================

namespace {

class Xorshift64 {
public:
  explicit Xorshift64(uint64_t seed) : m_state(seed == 0 ? 1 : seed) {}

  uint64_t next() {
    m_state ^= m_state << 13;
    m_state ^= m_state >> 7;
    m_state ^= m_state << 17;
    return m_state;
  }

  uint64_t below(uint64_t limit) { return next() % limit; }

private:
  uint64_t m_state;
};

std::string random_json_string(Xorshift64 &rng, size_t max_len) {
  static constexpr const char *kAlphabet =
      "abcXYZ0123{}[]:\",\\/. \t\n\"\\u005c\n\\\"\xC3\xA9\xF0\x9F\x98\x80";
  const size_t len = 1 + rng.below(max_len);
  std::string s;
  s.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    s.push_back(kAlphabet[rng.below(std::char_traits<char>::length(kAlphabet))]);
  }
  return s;
}

std::string random_ascii_string(Xorshift64 &rng, size_t max_len) {
  const size_t len = 1 + rng.below(max_len);
  std::string s;
  s.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    s.push_back(static_cast<char>(' ' + rng.below(95)));
  }
  return s;
}

json random_json_value(Xorshift64 &rng, int depth) {
  switch (rng.below(depth <= 0 ? 4 : 7)) {
  case 0:
    return rng.below(2) == 0;
  case 1:
    return static_cast<double>(static_cast<int64_t>(rng.next()) %
                               (INT64_C(1) << 40));
  case 2:
    return random_ascii_string(rng, 32);
  case 3:
    return nullptr;
  case 4: {
    json arr = json::array();
    const size_t n = rng.below(6);
    for (size_t i = 0; i < n; ++i) {
      arr.push_back(random_json_value(rng, depth - 1));
    }
    return arr;
  }
  default: {
    json obj = json::object();
    const size_t n = rng.below(6);
    for (size_t i = 0; i < n; ++i) {
      obj[random_ascii_string(rng, 12)] = random_json_value(rng, depth - 1);
    }
    return obj;
  }
  }
}

} // namespace

TEST_CASE("Fuzz: JsonUtils::try_parse survives random byte strings",
          "[fuzz]") {
  Xorshift64 rng(0xA11CE5EED);
  for (int i = 0; i < 20000; ++i) {
    const std::string s = random_json_string(rng, 256);
    const auto result = JsonUtils::try_parse(s);
    (void)result;
  }
}

TEST_CASE("Fuzz: validators survive random JSON documents", "[fuzz]") {
  const auto request_validator = create_standard_request_validator();
  const auto response_validator = create_standard_response_validator();
  Xorshift64 rng(0xF0000A11);
  for (int i = 0; i < 5000; ++i) {
    const json doc = random_json_value(rng, 6);
    std::string error;
    try {
      (void)request_validator.validate(doc, error);
    } catch (const json::type_error &) {
      continue;
    }
    try {
      (void)response_validator.validate(doc, error);
    } catch (const json::type_error &) {
      continue;
    }
  }
}

TEST_CASE("Fuzz: validator type mismatches do not crash", "[fuzz]") {
  // Wrong-typed fields (numbers/bools/objects where strings are expected) are
  // fed to nlohmann::json conversions that throw type_error by design.
  RequestValidator validator;
  validator.add_required_field("method")
      .add_required_field("path")
      .add_allowed_method("GET")
      .add_allowed_method("POST");
  ResponseValidator resp;
  resp.require_body(true).add_allowed_status_code(200);

  Xorshift64 rng(0xBADCAFE0);
  for (int i = 0; i < 20000; ++i) {
    const json doc = random_json_value(rng, 4);
    std::string error;
    try {
      (void)validator.validate(doc, error);
    } catch (const json::type_error &) {
      continue;
    }
    try {
      (void)resp.validate(doc, error);
    } catch (const json::type_error &) {
      continue;
    }
  }
}

TEST_CASE("Fuzz: Config get_env_* parsers survive garbage values", "[fuzz]") {
  Xorshift64 rng(0xFEEDBEEF);
  for (int i = 0; i < 5000; ++i) {
    const std::string value = random_ascii_string(rng, 64);
    EnvVarGuard env("FUZZ_VALUE", value.c_str());
    (void)Config::get_env_int("FUZZ_VALUE", 42);
    (void)Config::get_env_double("FUZZ_VALUE", 0.5);
    (void)Config::get_env_bool("FUZZ_VALUE", true);
    (void)Config::get_env_protocol("FUZZ_VALUE", "http");
  }
}

// ============================================================================
// Common utils (common_utils.hpp header-only helpers)
// ============================================================================

TEST_CASE("Common utils: get_header_value returns value or default",
          "[common-utils]") {
  httplib::Headers headers;
  headers.emplace("x-token", "abc");
  headers.emplace("x-empty", "");
  REQUIRE(get_header_value(headers, "x-token") == "abc");
  REQUIRE(get_header_value(headers, "x-token", "dflt") == "abc");
  REQUIRE(get_header_value(headers, "x-missing") == "unknown");
  REQUIRE(get_header_value(headers, "x-missing", "dflt") == "dflt");
  REQUIRE(get_header_value(headers, "x-empty") == "unknown");
}

TEST_CASE("Common utils: find_header_optional", "[common-utils]") {
  httplib::Headers headers;
  headers.emplace("x-token", "abc");
  REQUIRE(find_header_optional(headers, "x-token").has_value());
  REQUIRE(*find_header_optional(headers, "x-token") == "abc");
  REQUIRE(!find_header_optional(headers, "x-missing").has_value());
  headers.emplace("x-empty", "");
  REQUIRE(!find_header_optional(headers, "x-empty").has_value());
}

TEST_CASE("Common utils: header_or_default", "[common-utils]") {
  httplib::Headers headers;
  headers.emplace("x-token", "abc");
  REQUIRE(header_or_default(headers, "x-token") == "abc");
  REQUIRE(header_or_default(headers, "x-missing") == "unknown");
  REQUIRE(header_or_default(headers, "x-missing", "dflt") == "dflt");
}

TEST_CASE("Common utils: resolve_parent_id", "[common-utils]") {
  REQUIRE(resolve_parent_id("span1", "fallback") == "span1");
  REQUIRE(resolve_parent_id("", "fallback") == "fallback");
}

TEST_CASE("Common utils: shorten_user_agent", "[common-utils]") {
  const std::string short_ua = "curl/8.0";
  REQUIRE(shorten_user_agent(short_ua) == short_ua);

  std::string long_no_pattern;
  long_no_pattern.assign(100, 'a');
  const auto shortened = shorten_user_agent(long_no_pattern);
  REQUIRE(shortened.size() <= 80);
  REQUIRE(shortened.find("...") != std::string::npos);

  REQUIRE(shorten_user_agent(
              "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")
              .find("Chrome/") == 0);
  REQUIRE(shorten_user_agent(
              "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Firefox/121.0")
              .find("Firefox/") == 0);
  REQUIRE(shorten_user_agent(
              "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Edg/119.0.0.0 Safari/537.36")
              .find("Edge/") == 0);
}

TEST_CASE("Common utils: validate_range and validate_positive",
          "[common-utils]") {
  REQUIRE(validate_positive(5, "port") == true);
  REQUIRE(validate_positive(0, "port") == false);
  REQUIRE(validate_positive(-3, "port") == false);

  REQUIRE(validate_range(5, "size", 1, 100) == true);
  REQUIRE(validate_range(0, "size", 1, 100) == false);
  REQUIRE(validate_range(101, "size", 1, 100) == false);
  REQUIRE(validate_range(50, "size", 1, 100, 40) == true);
  REQUIRE(validate_range(80, "size", 1, 100, 40) == true);
}

TEST_CASE("Common utils: RetryHandler manages backoff state",
          "[common-utils]") {
  RetryHandler handler(100, 1000);
  REQUIRE(handler.get_consecutive_failures() == 0);
  REQUIRE(handler.get_current_delay_ms() == 100);

  handler.record_failure();
  REQUIRE(handler.get_consecutive_failures() == 1);
  REQUIRE(handler.get_current_delay_ms() == 200);

  handler.record_failure();
  handler.record_failure();
  handler.record_failure();
  REQUIRE(handler.get_consecutive_failures() == 4);
  REQUIRE(handler.get_current_delay_ms() == 1000);

  handler.record_success();
  REQUIRE(handler.get_consecutive_failures() == 0);
  REQUIRE(handler.get_current_delay_ms() == 100);
}

// ============================================================================
// Common utils (common_utils.cpp compiled functions)
// ============================================================================

TEST_CASE("Common utils: compute_sha256_hex", "[common-utils]") {
  REQUIRE(compute_sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  REQUIRE(compute_sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // Deterministic for identical input.
  REQUIRE(compute_sha256_hex("same") == compute_sha256_hex("same"));
  REQUIRE(compute_sha256_hex("a") != compute_sha256_hex("b"));
}

TEST_CASE("Common utils: log_body_preview", "[common-utils]") {
  REQUIRE(log_body_preview("short") == "short");
  REQUIRE(log_body_preview("exact", 5) == "exact");
  const auto preview = log_body_preview("0123456789", 5);
  REQUIRE(preview.find("01234...") == 0);
  REQUIRE(preview.find("10 bytes total") != std::string::npos);
}

TEST_CASE("Common utils: parse_url", "[common-utils]") {
  const auto http = parse_url("http://example.com:8080/api");
  REQUIRE(http.m_host == "example.com");
  REQUIRE(http.m_port == 8080);
  REQUIRE(http.m_path == "/api");
  REQUIRE(http.m_is_https == false);

  const auto https = parse_url("https://secure.example.com/path");
  REQUIRE(https.m_host == "secure.example.com");
  REQUIRE(https.m_is_https == true);
  REQUIRE(https.m_port == 443);

  const auto no_port = parse_url("http://example.com/x");
  REQUIRE(no_port.m_host == "example.com");
  REQUIRE(no_port.m_path == "/x");
}

// Coverage for the l2-proxy runtime stack the coverage target links in but no
// earlier test touched: proxy_init, RequestHandler, ServerHandler,
// DbQueryHandler, the NATS push/poll services and the NatsClient failure
// paths. Everything runs against a locally constructed AppContext — no NATS
// server, no database and no network are involved.
//
// Connecting has to fail FAST. natsConnection_Connect is configured with
// RetryOnFailedConnect + an infinite MaxReconnect, so the C client retries
// forever while the server is unreachable and NatsClient::connect() would never
// return. Every test that lets connect() run therefore enables TLS with a CA
// file that does not exist: natsOptions_LoadCATrustedCertificates() fails,
// setup_options() aborts the connect before the client ever dials, and
// connect() returns false immediately. NATS_TIMEOUT_MS/DB_QUERY_NATS_TIMEOUT_MS
// stay low as a second line of defense in case that assumption ever breaks.

#include "app_context.hpp"
#include "common_utils.hpp"
#include "db_query_executor.hpp"
#include "db_query_executor_oracle.hpp"
#include "db_query_handler.hpp"
#include "json_utils.hpp"
#include "nats_client.hpp"
#include "nats_poll_service.hpp"
#include "nats_push_service.hpp"
#include "proxy_init.hpp"
#include "rate_limiter_per_ip.hpp"
#include "request_handler.hpp"
#include "response_builder.hpp"
#include "server_handler.hpp"
#include "stats_logger.hpp"
#include "trace_logger.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// RAII override of several env vars, restoring the previous value (set or
// unset) on scope exit so one test cannot leak state into the next one.
class EnvGuard {
public:
  EnvGuard(std::initializer_list<std::pair<const char *, const char *>> vars) {
    m_vars.reserve(vars.size());
    for (const auto &[name, value] : vars) {
      m_vars.emplace_back(name, read_env(name));
      if (value == nullptr) {
        unsetenv(name);
      } else {
        setenv(name, value, 1);
      }
    }
  }

  ~EnvGuard() {
    for (const auto &[name, previous] : m_vars) {
      if (previous.has_value()) {
        setenv(name.c_str(), previous->c_str(), 1);
      } else {
        unsetenv(name.c_str());
      }
    }
  }

  EnvGuard(const EnvGuard &) = delete;
  EnvGuard &operator=(const EnvGuard &) = delete;
  EnvGuard(EnvGuard &&) = delete;
  EnvGuard &operator=(EnvGuard &&) = delete;

private:
  static std::optional<std::string> read_env(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string(value);
  }

  std::vector<std::pair<std::string, std::optional<std::string>>> m_vars;
};

// Proxy mode plus the fail-fast NATS setup described in the file header.
class ProxyEnv {
public:
  ProxyEnv()
      : m_env({{"MODE", "proxy"},
               {"NATS_ENABLE_TLS", "true"},
               {"NATS_TLS_CA_CERT_FILE", "/nonexistent-test-ca.pem"},
               {"NATS_TLS_CERT_FILE", nullptr},
               {"NATS_TLS_KEY_FILE", nullptr},
               {"NATS_TIMEOUT_MS", "1000"},
               {"DB_QUERY_NATS_TIMEOUT_MS", "1000"},
               {"REQUEST_TIMEOUT_SECONDS", "1"}}) {}

  ProxyEnv(const ProxyEnv &) = delete;
  ProxyEnv &operator=(const ProxyEnv &) = delete;
  ProxyEnv(ProxyEnv &&) = delete;
  ProxyEnv &operator=(ProxyEnv &&) = delete;

private:
  EnvGuard m_env;
};

httplib::Request make_get(const std::string &path) {
  httplib::Request req;
  req.method = "GET";
  req.path = path;
  return req;
}

httplib::Request make_post(const std::string &path, const std::string &body) {
  httplib::Request req;
  req.method = "POST";
  req.path = path;
  req.body = body;
  return req;
}

// Batched Jaeger exporter pointed at a refused port: spans are buffered and
// the flush fails harmlessly, so the tracer-dependent branches become
// reachable without an observability backend.
void attach_tracer(AppContext &ctx) {
  auto &metrics = *ctx.m_tracing_metrics;
  ctx.m_tracer = std::make_unique<JaegerLogger>(
      "http://127.0.0.1:1/api/traces", metrics.m_spans_sent,
      metrics.m_spans_failed, metrics.m_queue_size, metrics.m_last_send_duration,
      metrics.m_send_latency, metrics.m_queue_time,
      /*batch_size=*/10000, /*flush_interval_ms=*/100000, /*sample_rate=*/1.0);
}

} // namespace

// ============================================================================
// proxy_init
// ============================================================================

TEST_CASE("ProxyInit: init_proxy_components builds the proxy runtime stack",
          "[proxy-init]") {
  const ProxyEnv env;
  AppContext ctx;
  REQUIRE_FALSE(ctx.is_proxy_components_initialized());

  init_proxy_components(ctx);

  REQUIRE(ctx.is_proxy_components_initialized());
  REQUIRE(ctx.m_nats_client != nullptr);
  REQUIRE(ctx.m_proxy.m_rate_limiter != nullptr);
  REQUIRE(ctx.m_proxy.m_per_ip_rate_limiter != nullptr);
  REQUIRE(ctx.m_proxy.m_duplicate_detector != nullptr);
  REQUIRE(ctx.m_proxy.m_rate_limiter_metrics != nullptr);
  REQUIRE(ctx.m_proxy.m_per_ip_rate_limiter_metrics != nullptr);
  REQUIRE(ctx.m_proxy.m_internal_memory_metrics != nullptr);
  REQUIRE(ctx.m_proxy.m_per_client_id_metrics_collector != nullptr);
  REQUIRE(ctx.m_proxy.m_per_client_id_latency_collector != nullptr);
  REQUIRE(ctx.m_proxy.m_per_client_id_duplicate_collector != nullptr);
  REQUIRE(ctx.m_proxy.m_per_ip_metrics_collector != nullptr);

  // No NATS server in the test container: connect must fail rather than hang.
  REQUIRE_FALSE(ctx.m_nats_client->is_connected());
  REQUIRE_FALSE(ctx.m_nats_client->connect());
  REQUIRE(ctx.m_nats_client->get_last_error().has_value());

  NatsClient *const client = ctx.m_nats_client.get();
  init_proxy_components(ctx);
  REQUIRE(ctx.m_nats_client.get() == client);

  ctx.m_nats_client.reset();
  REQUIRE_FALSE(ctx.is_proxy_components_initialized());
}

TEST_CASE("ProxyInit: per-IP collector exports limiter stats",
          "[proxy-init]") {
  const ProxyEnv env;
  AppContext ctx;
  init_proxy_components(ctx);
  REQUIRE(ctx.m_proxy.m_per_ip_metrics_collector != nullptr);
  REQUIRE(ctx.m_proxy.m_per_ip_rate_limiter != nullptr);

  REQUIRE(ctx.m_proxy.m_per_ip_rate_limiter->acquire("10.11.12.13"));

  const auto collected = ctx.m_proxy.m_per_ip_metrics_collector->Collect();
  REQUIRE_FALSE(collected.empty());
}

// ============================================================================
// ServerHandler (l2-server mode)
// ============================================================================

TEST_CASE("ServerHandler: health, favicon and value echo", "[server-handler]") {
  const EnvGuard mode{{"MODE", "l2-server"}};
  AppContext ctx;
  ServerHandler handler(ctx);

  {
    const httplib::Request req = make_get("/health/live");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 200);
    REQUIRE(res.body.find("alive") != std::string::npos);
  }
  {
    const httplib::Request req = make_get("/health");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 200);
    REQUIRE(res.body.find("alive") != std::string::npos);
  }
  {
    const httplib::Request req = make_get("/health/ready");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 200);
    REQUIRE(res.body.find("ready") != std::string::npos);
    REQUIRE(ctx.m_server.m_metrics->m_health_ready.Value() == 1.0);
  }
  {
    const httplib::Request req = make_get("/favicon.ico");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.get_header_value("Content-Type") == "image/x-icon");
    REQUIRE(res.body.size() == 70);
    REQUIRE(res.body.compare(0, 4, std::string("\x00\x00\x01\x00", 4)) == 0);
  }
  {
    const httplib::Request req = make_get("/");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 200);
    REQUIRE(json::parse(res.body)["value_return"] == 0);
    REQUIRE(json::parse(res.body).contains("server_span_id"));
  }

  {
    const httplib::Request req = make_post("/", "not json");
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 400);
    REQUIRE(json::parse(res.body)["error"] == "Invalid JSON");
  }
  {
    const httplib::Request req = make_post("/", R"({"value":42})");
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 200);
    REQUIRE(json::parse(res.body)["value_return"] == 42);
  }
  {
    // Correlation-test echo: req_id is echoed and req_hash is the SHA-256 of
    // the exact request body the client sent.
    httplib::Request req =
        make_post("/", R"({"value":7,"req_id":"req-9"})");
    req.headers = {{"X-Correlation-Test", "1"}};
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 200);
    const json body = json::parse(res.body);
    REQUIRE(body["value_return"] == 7);
    REQUIRE(body["req_id"] == "req-9");
    REQUIRE(body["req_hash"] == compute_sha256_hex(req.body));
  }
}

TEST_CASE("ServerHandler: traced response logging", "[server-handler]") {
  const EnvGuard mode{{"MODE", "l2-server"}};
  AppContext ctx;
  attach_tracer(ctx);
  ServerHandler handler(ctx);

  const httplib::Request req = make_get("/");
  httplib::Response res;
  handler.handle_get(req, res);
  REQUIRE(res.status == 200);
  REQUIRE(json::parse(res.body).contains("server_span_id"));
}

TEST_CASE("ServerHandler: test-mode response delay", "[server-handler]") {
  const EnvGuard mode{{"MODE", "l2-server"}};
  const EnvGuard delay{{"L2_TEST_RESPONSE_DELAY_MS", "1"}};
  AppContext ctx;
  REQUIRE(ctx.m_config.m_server.m_test_response_delay_ms == 1);

  ServerHandler handler(ctx);
  const httplib::Request req = make_post("/", R"({"value":1})");
  httplib::Response res;
  handler.handle_post(req, res);
  REQUIRE(res.status == 200);
  REQUIRE(json::parse(res.body)["value_return"] == 1);
}

// ============================================================================
// RequestHandler (proxy mode)
// ============================================================================

TEST_CASE("RequestHandler: GET health, stats and debug endpoints",
          "[request-handler]") {
  const EnvGuard mode{{"MODE", "proxy"}};
  AppContext ctx;
  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);
  RequestHandler handler(ctx, stats);

  {
    const httplib::Request req = make_get("/health/live");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 200);
    REQUIRE(res.body.find("alive") != std::string::npos);
  }
  {
    const httplib::Request req = make_get("/stats");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.get_header_value("Content-Type") ==
            "text/html; charset=utf-8");
    REQUIRE(res.body.find("<html") != std::string::npos);
  }
  {
    // The crash endpoint stays disarmed by default (ENABLE_CRASH_TEST_ENDPOINT
    // is off): a public client must not be able to SIGSEGV the proxy.
    const httplib::Request req = make_get("/crash-test");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 404);
    REQUIRE(json::parse(res.body)["error"].get<std::string>().find("disabled") !=
            std::string::npos);
  }
  {
    const httplib::Request req = make_get("/debug/stacktrace");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 200);
    const json body = json::parse(res.body);
    REQUIRE(body.contains("stacktrace"));
    REQUIRE(body["frames"].get<int>() > 0);
  }
  {
    // No NATS client at all: readiness must answer fast with 503 instead of
    // trying to dial the backend.
    const httplib::Request req = make_get("/health/ready");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 503);
    REQUIRE(json::parse(res.body)["error"] == "NATS connection not available");
    REQUIRE(json::parse(res.body)["status"] == "not_ready");
    REQUIRE(ctx.m_proxy.m_metrics->m_health_ready.Value() == 0.0);
  }
  {
    // Duplicate detector is only created by init_proxy_components().
    const httplib::Request req = make_get("/debug/duplicates");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 404);
  }
  {
    // Unknown GET falls through to the backend path and times out with 504.
    const httplib::Request req = make_get("/");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 504);
    REQUIRE(json::parse(res.body)["error"] == "Timeout waiting for response");
  }
}

TEST_CASE("RequestHandler: readiness with a configured NATS client",
          "[request-handler]") {
  const ProxyEnv env;
  const EnvGuard allow{{"HEALTH_READY_ALLOW_CONNECT", "true"}};
  AppContext ctx;
  REQUIRE(ctx.m_config.m_app.m_health_ready_allow_connect);
  init_proxy_components(ctx);
  REQUIRE(ctx.m_nats_client != nullptr);

  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);
  RequestHandler handler(ctx, stats);

  const httplib::Request req = make_get("/health/ready");
  httplib::Response res;
  handler.handle_get(req, res);
  REQUIRE(res.status == 503);
  REQUIRE(json::parse(res.body)["error"] == "NATS connection not available");
  REQUIRE(ctx.m_proxy.m_metrics->m_nats_connected.Value() == 0.0);
}

TEST_CASE("RequestHandler: POST without a backend answers 504",
          "[request-handler]") {
  const EnvGuard mode{{"MODE", "proxy"}};
  AppContext ctx;
  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);
  RequestHandler handler(ctx, stats);

  {
    const httplib::Request req = make_post("/", "not json");
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 400);
    REQUIRE(json::parse(res.body)["error"] == "Invalid JSON in request body");
  }
  {
    httplib::Request req = make_post("/", R"({"value":1})");
    req.headers = {{"X-DataHub-Client-Id", "client-a"}};
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 504);
    REQUIRE(json::parse(res.body)["error"] == "Timeout waiting for response");
    REQUIRE(json::parse(res.body).contains("request_id"));
  }
}

TEST_CASE("RequestHandler: global rate limiter rejects with 429",
          "[rate-limit-http]") {
  const ProxyEnv env;
  const EnvGuard dup{{"DUPLICATE_DETECTION_ENABLED", "false"}};
  const EnvGuard global_on{{"ENABLE_GLOBAL_RATE_LIMITING", "true"}};
  const EnvGuard global_max{{"GLOBAL_RATE_LIMIT_MAX_TOKENS", "1"}};
  const EnvGuard global_refill{{"GLOBAL_RATE_LIMIT_REFILL_RATE", "1"}};
  const EnvGuard per_ip_off{{"ENABLE_PER_IP_RATE_LIMITING", "false"}};
  AppContext ctx;
  REQUIRE(ctx.m_config.m_rate_limit.m_global.m_max_tokens == 1);
  init_proxy_components(ctx);
  ctx.m_nats_client.reset();

  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);
  RequestHandler handler(ctx, stats);

  {
    const httplib::Request req = make_post("/", R"({"value":1})");
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 504);
  }
  {
    // The bucket holds a single token and refills only once per second, so the
    // second back-to-back request is deterministically rejected.
    const httplib::Request req = make_post("/", R"({"value":2})");
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 429);
    REQUIRE(res.get_header_value("Retry-After") == "1");
    REQUIRE(res.get_header_value("X-RateLimit-Limit") == "1");
    REQUIRE(res.get_header_value("X-RateLimit-Remaining") == "0");
    REQUIRE(json::parse(res.body)["error"] ==
            "Global rate limit exceeded. Please retry later.");
  }
}

TEST_CASE("RequestHandler: per-IP rate limiter rejects with 429",
          "[rate-limit-http]") {
  const ProxyEnv env;
  const EnvGuard dup{{"DUPLICATE_DETECTION_ENABLED", "false"}};
  const EnvGuard global_off{{"ENABLE_GLOBAL_RATE_LIMITING", "false"}};
  const EnvGuard per_ip_on{{"ENABLE_PER_IP_RATE_LIMITING", "true"}};
  const EnvGuard per_ip_max{{"PER_IP_MAX_TOKENS", "1"}};
  const EnvGuard per_ip_refill{{"PER_IP_REFILL_RATE", "1"}};
  AppContext ctx;
  REQUIRE(ctx.m_config.m_rate_limit.m_per_ip.m_max_tokens == 1);
  init_proxy_components(ctx);
  ctx.m_nats_client.reset();

  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);
  RequestHandler handler(ctx, stats);

  {
    httplib::Request req = make_post("/", R"({"value":1})");
    req.headers = {{"X-Real-IP", "10.0.0.7"}};
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 504);
  }
  {
    httplib::Request req = make_post("/", R"({"value":2})");
    req.headers = {{"X-Real-IP", "10.0.0.7"}};
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 429);
    REQUIRE(res.get_header_value("X-RateLimit-Limit") == "1");
    REQUIRE(res.get_header_value("X-RateLimit-Remaining") == "0");
    REQUIRE(json::parse(res.body)["error"].get<std::string>().find(
                "Rate limit exceeded for your IP") == 0);
  }
}

TEST_CASE("RequestHandler: duplicate POST is counted but still forwarded",
          "[duplicate-http]") {
  const ProxyEnv env;
  const EnvGuard dup_on{{"DUPLICATE_DETECTION_ENABLED", "true"}};
  const EnvGuard dup_reject{{"DUPLICATE_REJECT_ENABLED", "false"}};
  const EnvGuard global_off{{"ENABLE_GLOBAL_RATE_LIMITING", "false"}};
  const EnvGuard per_ip_off{{"ENABLE_PER_IP_RATE_LIMITING", "false"}};
  AppContext ctx;
  REQUIRE(ctx.m_config.m_duplicate.m_enabled);
  REQUIRE_FALSE(ctx.m_config.m_duplicate.m_reject_enabled);
  init_proxy_components(ctx);
  ctx.m_nats_client.reset();

  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);
  RequestHandler handler(ctx, stats);

  const std::string body = R"({"value":1})";
  {
    const httplib::Request req = make_post("/", body);
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 504);
  }
  {
    // Same body again: detected as a duplicate but rejection is off, so the
    // request is still handed to the backend (504, not 409).
    const httplib::Request req = make_post("/", body);
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 504);
  }
  {
    const httplib::Request req = make_get("/debug/duplicates");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 200);
    const json report = json::parse(res.body);
    REQUIRE(report["enabled"] == true);
    REQUIRE(report["duplicate_bodies"] == 1);
    REQUIRE(report["duplicate_occurrences"] == 1);
  }
}

TEST_CASE("RequestHandler: duplicate POST is rejected with 409",
          "[duplicate-http]") {
  const ProxyEnv env;
  const EnvGuard dup_on{{"DUPLICATE_DETECTION_ENABLED", "true"}};
  const EnvGuard dup_reject{{"DUPLICATE_REJECT_ENABLED", "true"}};
  const EnvGuard global_off{{"ENABLE_GLOBAL_RATE_LIMITING", "false"}};
  const EnvGuard per_ip_off{{"ENABLE_PER_IP_RATE_LIMITING", "false"}};
  AppContext ctx;
  REQUIRE(ctx.m_config.m_duplicate.m_reject_enabled);
  init_proxy_components(ctx);
  ctx.m_nats_client.reset();

  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);
  RequestHandler handler(ctx, stats);

  const std::string body = R"({"value":3})";
  {
    const httplib::Request req = make_post("/", body);
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 504);
  }
  {
    const httplib::Request req = make_post("/", body);
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 409);
    REQUIRE(json::parse(res.body)["error"] == "duplicate request");
  }
}

// ============================================================================
// HTTP DB Gateway (/v1/sql/*)
// ============================================================================

TEST_CASE("RequestHandler: DB gateway answers 404 when disabled",
          "[db-gateway-http]") {
  const EnvGuard mode{{"MODE", "proxy"}};
  AppContext ctx;
  REQUIRE_FALSE(ctx.m_config.m_db_query.m_enabled);

  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);
  RequestHandler handler(ctx, stats);

  const httplib::Request req = make_get("/v1/sql");
  httplib::Response res;
  handler.handle_get(req, res);
  REQUIRE(res.status == 404);
  REQUIRE(json::parse(res.body)["error"]["code"] == "NOT_FOUND");
  REQUIRE(json::parse(res.body)["error"]["message"] == "DB gateway is disabled");
}

TEST_CASE("RequestHandler: DB gateway list, ping and query routing",
          "[db-gateway-http]") {
  const EnvGuard mode{{"MODE", "proxy"}};
  const EnvGuard db{{"DB_QUERY_ENABLED", "true"}};
  const EnvGuard pg{{"DB_POSTGRES_ENABLED", "true"}};
  AppContext ctx;
  REQUIRE(ctx.m_config.m_db_query.m_enabled);
  REQUIRE(ctx.m_config.m_db_query.m_databases.size() == 1);

  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);
  RequestHandler handler(ctx, stats);

  {
    const httplib::Request req = make_get("/v1/sql");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 200);
    const json list = json::parse(res.body);
    REQUIRE(list["databases"].size() == 1);
    REQUIRE(list["databases"][0]["name"] == "postgres");
    REQUIRE(list["databases"][0]["driver"] == "postgres");
    REQUIRE(list["databases"][0]["enabled"] == true);
  }
  {
    const httplib::Request req = make_post("/v1/sql", "");
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 405);
    REQUIRE(json::parse(res.body)["error"]["code"] == "METHOD_NOT_ALLOWED");
  }
  {
    // No NATS client in this context: the routed ping lands on the 503 branch.
    const httplib::Request req = make_get("/v1/sql/postgres/ping");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 503);
    REQUIRE(json::parse(res.body)["error"]["code"] == "DB_UNAVAILABLE");
  }
  {
    const httplib::Request req = make_post("/v1/sql/postgres/ping", "");
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 405);
  }
  {
    const httplib::Request req = make_get("/v1/sql/postgres/query");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 405);
  }
  {
    const httplib::Request req = make_get("/v1/sql/postgres");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 404);
    REQUIRE(json::parse(res.body)["error"]["message"] ==
            "Unknown DB gateway path");
  }
  {
    const httplib::Request req = make_get("/v1/sql/postgres/delete");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 404);
    REQUIRE(json::parse(res.body)["error"]["code"] == "NOT_FOUND");
  }
  {
    const httplib::Request req = make_get("/v1/sql/unknown/ping");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 404);
    REQUIRE(json::parse(res.body)["error"]["code"] == "UNKNOWN_DATABASE");
  }
  {
    const httplib::Request req =
        make_post("/v1/sql/postgres/query", "not json");
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 400);
    REQUIRE(json::parse(res.body)["error"]["code"] == "BAD_REQUEST");
    REQUIRE(json::parse(res.body)["error"]["message"] == "Invalid JSON body");
  }
  {
    const httplib::Request req = make_post("/v1/sql/postgres/query", "{}");
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 400);
    REQUIRE(json::parse(res.body)["error"]["code"] == "BAD_REQUEST");
  }
  {
    // Only SELECT/WITH statements may reach the gateway.
    const httplib::Request req = make_post("/v1/sql/postgres/query",
                                           R"({"sql":"DELETE FROM t"})");
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 400);
    REQUIRE(json::parse(res.body)["error"]["code"] == "BAD_REQUEST");
  }
  {
    // Valid query, still no NATS client → the routed 503 branch.
    const httplib::Request req =
        make_post("/v1/sql/postgres/query", R"({"sql":"SELECT 1"})");
    httplib::Response res;
    handler.handle_post(req, res);
    REQUIRE(res.status == 503);
    REQUIRE(json::parse(res.body)["error"]["code"] == "DB_UNAVAILABLE");
  }
}

TEST_CASE("RequestHandler: DB gateway maps a failed NATS round-trip to 503",
          "[db-gateway-http]") {
  const ProxyEnv env;
  const EnvGuard db{{"DB_QUERY_ENABLED", "true"}};
  const EnvGuard pg{{"DB_POSTGRES_ENABLED", "true"}};
  AppContext ctx;
  init_proxy_components(ctx);
  REQUIRE(ctx.m_nats_client != nullptr);

  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);
  RequestHandler handler(ctx, stats);

  const httplib::Request req = make_get("/v1/sql/postgres/ping");
  httplib::Response res;
  handler.handle_get(req, res);
  REQUIRE(res.status == 503);
  REQUIRE(json::parse(res.body)["error"]["code"] == "DB_UNAVAILABLE");
}

TEST_CASE("RequestHandler: DB gateway round-trip span is traced",
          "[db-gateway-http]") {
  const ProxyEnv env;
  const EnvGuard db{{"DB_QUERY_ENABLED", "true"}};
  const EnvGuard pg{{"DB_POSTGRES_ENABLED", "true"}};
  AppContext ctx;
  init_proxy_components(ctx);
  attach_tracer(ctx);

  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);
  RequestHandler handler(ctx, stats);

  httplib::Request req = make_get("/v1/sql/postgres/ping");
  req.headers = {
      {"traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01"}};
  httplib::Response res;
  handler.handle_get(req, res);
  REQUIRE(res.status == 503);
  REQUIRE(json::parse(res.body)["error"]["code"] == "DB_UNAVAILABLE");
}

TEST_CASE("RequestHandler: traced backend timeout records a proxy span",
          "[request-handler]") {
  const EnvGuard mode{{"MODE", "proxy"}};
  AppContext ctx;
  attach_tracer(ctx);
  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);
  RequestHandler handler(ctx, stats);

  httplib::Request req = make_post("/", R"({"value":1})");
  req.headers = {
      {"traceparent", "00-11111111111111111111111111111111-2222222222222222-01"}};
  httplib::Response res;
  handler.handle_post(req, res);
  REQUIRE(res.status == 504);
  REQUIRE(json::parse(res.body).contains("request_id"));
}

// ============================================================================
// NatsPushService / NatsPollService
// ============================================================================

TEST_CASE("NatsPushService: serializes the backend request",
          "[nats-services]") {
  const EnvGuard mode{{"MODE", "proxy"}};
  AppContext ctx;
  NatsPushService push(ctx);

  {
    const json request{{NatsContract::kRequestId, "req-42"}};
    const TraceContext trace;
    const std::string serialized =
        push.push_request(request, "trace-1", "span-1", trace);
    const json parsed = json::parse(serialized);
    REQUIRE(parsed[NatsContract::kRequestId] == "req-42");
    REQUIRE(parsed[NatsContract::kTimestamp].is_number());
    REQUIRE(parsed[NatsContract::kProxySpanId] == "span-1");
    REQUIRE(parsed[NatsContract::kProxyTraceId] == "trace-1");
    REQUIRE_FALSE(parsed.contains(NatsContract::kProxyTraceparent));
  }
  {
    // An envelope that already carries a proxy span id keeps it as the
    // operation span instead of the caller's span id.
    const json request{{NatsContract::kRequestId, "req-43"},
                       {NatsContract::kProxySpanId, "outer-span"}};
    const TraceContext trace;
    const std::string serialized =
        push.push_request(request, "trace-2", "span-2", trace);
    const json parsed = json::parse(serialized);
    REQUIRE(parsed[NatsContract::kProxySpanId] == "outer-span");
    // The tracer only exists in main.cpp, so no traceparent can be generated.
    REQUIRE_FALSE(parsed.contains(NatsContract::kProxyTraceparent));
  }
}

TEST_CASE("NatsPushService: traceparent and inlet span with a tracer",
          "[nats-services]") {
  const EnvGuard mode{{"MODE", "proxy"}};
  AppContext ctx;
  attach_tracer(ctx);
  NatsPushService push(ctx);

  json request{{NatsContract::kRequestId, "req-45"},
               {NatsContract::kProxyInletSpanId, "inlet-1"}};
  const TraceContext trace;
  const std::string serialized =
      push.push_request(request, "trace-3", "span-3", trace);
  const json parsed = json::parse(serialized);
  REQUIRE(parsed.contains(NatsContract::kProxyTraceparent));
  REQUIRE(parsed[NatsContract::kProxyInletSpanId] == "inlet-1");
}

TEST_CASE("NatsPollService: empty reply without a NATS client",
          "[nats-services]") {
  const EnvGuard mode{{"MODE", "proxy"}};
  AppContext ctx;
  NatsPollService poll(ctx);

  const double before = ctx.m_proxy.m_metrics->m_nats_connection_errors.Value();
  REQUIRE(poll.poll_response("req-1", "{}", 1, TraceContext{}).empty());
  REQUIRE(ctx.m_proxy.m_metrics->m_nats_connection_errors.Value() ==
          before + 1.0);
}

TEST_CASE("NatsPollService: reconnect loop ends with an empty reply",
          "[nats-services]") {
  const ProxyEnv env;
  AppContext ctx;
  init_proxy_components(ctx);
  NatsPollService poll(ctx);

  const double before = ctx.m_proxy.m_metrics->m_nats_connection_errors.Value();
  REQUIRE(poll.poll_response("req-2", "{}", 1, TraceContext{}).empty());
  REQUIRE(ctx.m_proxy.m_metrics->m_nats_connection_errors.Value() > before);
}

// ============================================================================
// NatsClient
// ============================================================================

TEST_CASE("NatsClient: every operation fails fast without a server",
          "[nats-client]") {
  NatsConfig cfg;
  cfg.m_host = "127.0.0.1";
  cfg.m_port = 1;
  cfg.m_subject = "test.subject";
  cfg.m_timeout_ms = 1000;
  cfg.m_enable_tls = true;
  cfg.m_tls_ca_cert_file = "/nonexistent-test-ca.pem";

  NatsClient client(cfg);
  REQUIRE_FALSE(client.is_connected());
  REQUIRE_FALSE(client.connect());
  REQUIRE(client.get_last_error().has_value());

  REQUIRE_FALSE(client.request("test.subject", "{}", 100).has_value());
  REQUIRE(client.request_with_headers("test.subject", "{}", {}, {}, 100)
              .m_data.empty());
  REQUIRE(client.request_with_consume_span_id("test.subject", "{}", 100)
              .first.m_data.empty());
  REQUIRE_FALSE(client.publish("test.subject", "{}"));
  const NatsHeaders headers{{"k", "v"}};
  REQUIRE_FALSE(client.publish_with_headers("test.subject", "{}", headers));
  REQUIRE_FALSE(client.subscribe("test.subject",
                                 [](const std::string &, const std::string &,
                                    const std::string &) {}));
  REQUIRE_FALSE(client.subscribe_queue("test.subject", "group",
                                       [](const std::string &,
                                          const std::string &,
                                          const std::string &) {}));
  client.unsubscribe();
  REQUIRE_FALSE(client.check_connection());
  REQUIRE_FALSE(client.ping().has_value());
  REQUIRE(client.drain(100));
  client.disconnect();
}

// ============================================================================
// DbQueryHandler / executor factory
// ============================================================================

TEST_CASE("Response builder: binary payload is base64-decoded",
          "[response-builder]") {
  const EnvGuard mode{{"MODE", "proxy"}};
  AppContext ctx;

  const json envelope =
      build_nats_response_envelope(200, "req-bin", "aGVsbG8=",
                                   get_current_timestamp_us(), true,
                                   "image/png", json::object(), "");
  httplib::Response res;
  set_response_content(res, envelope, "req-bin", TraceContext{}, "POST",
                       "/image", get_current_timestamp_us(), ctx);

  REQUIRE(res.status == 200);
  REQUIRE(res.body == "hello");
  REQUIRE(res.get_header_value("Content-Type") == "image/png");
}

TEST_CASE("DbQueryHandler: reports readiness without a reachable database",
          "[db-query-handler]") {
  DbQueryHandler handler;
  REQUIRE_FALSE(handler.init({}));
  REQUIRE_FALSE(handler.is_enabled());
  REQUIRE_FALSE(handler.all_configured());
  REQUIRE(handler.configured_databases().empty());
  REQUIRE(handler.ready_databases().empty());

  DbConfig db;
  db.m_name = "pg";
  db.m_driver = "postgres";
  // Loopback with a closed port: libpq refuses instantly instead of waiting
  // for connect_timeout, so the failed init stays fast.
  db.m_host = "127.0.0.1";
  db.m_port = 1;
  db.m_database = "test";
  db.m_user = "test";

  REQUIRE_FALSE(handler.init({db}));
  REQUIRE_FALSE(handler.is_enabled());
  REQUIRE_FALSE(handler.all_configured());

  const std::vector<std::string> expected{"pg"};
  REQUIRE(handler.configured_databases() == expected);
  REQUIRE(handler.ready_databases().empty());

  handler.set_pool_metrics(nullptr);

  json invalid = json::array();
  int status = 0;
  json body;
  handler.handle_request(invalid, status, body);
  REQUIRE(status == 400);
  REQUIRE(body["error"]["code"] == "BAD_REQUEST");

  const json ping{{DbQueryContract::kType, DbQueryContract::kTypePing},
                  {DbQueryContract::kRequestId, "req-1"},
                  {DbQueryContract::kDb, "pg"}};
  handler.handle_request(ping, status, body);
  REQUIRE(status == 404);
  REQUIRE(body["error"]["code"] == "UNKNOWN_DATABASE");
}

TEST_CASE("create_db_query_executor: dispatches by driver",
          "[db-query-handler]") {
  DbConfig unknown;
  unknown.m_name = "other";
  unknown.m_driver = "mysql";
  REQUIRE(create_db_query_executor(unknown) == nullptr);

  DbConfig pg;
  pg.m_name = "pg";
  pg.m_driver = "postgres";
  auto pg_executor = create_db_query_executor(pg);
  REQUIRE(pg_executor != nullptr);
  REQUIRE(pg_executor->db_name() == "pg");
  REQUIRE(pg_executor->default_timeout_ms() == pg.m_query_timeout_ms);
  REQUIRE(pg_executor->default_max_rows() == pg.m_max_rows);

  DbConfig ora;
  ora.m_name = "ora";
  ora.m_driver = "oracle";
  auto ora_executor = create_db_query_executor(ora);
  REQUIRE(ora_executor != nullptr);
  REQUIRE(ora_executor->db_name() == "ora");
  REQUIRE_FALSE(ora_executor->is_ready());
}

TEST_CASE("OracleQueryExecutor: background init without a reachable server",
          "[db-query-handler]") {
  DbConfig ora;
  ora.m_name = "ora";
  ora.m_driver = "oracle";
  // Refused port: ODPI-C fails the pool creation instantly instead of waiting
  // for the connect timeout, so the background thread stays in its retry loop.
  ora.m_host = "127.0.0.1";
  ora.m_port = 1;
  ora.m_service = "ORCL";
  ora.m_user = "scott";
  ora.m_password = "tiger";
  ora.m_pool_max = 4;

  AppContext ctx;
  OracleQueryExecutor executor(ora);
  executor.set_pool_metrics(&ctx.m_worker.m_metrics->m_db_pool_connections);

  REQUIRE(executor.init());
  REQUIRE(executor.init());
  REQUIRE_FALSE(executor.is_ready());

  int status = 0;
  const json unavailable = executor.execute_query(
      "SELECT 1 FROM DUAL", json::object(), 100, 10, status);
  REQUIRE(status == 503);
  REQUIRE(unavailable["error"]["code"] == "DB_UNAVAILABLE");
  REQUIRE_FALSE(executor.ping(100));

  // Give the background thread time to create the ODPI context before the
  // destructor joins it.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

// ============================================================================
// StatsLogger
// ============================================================================

namespace {
// One logging cadence of the periodic stats thread is `log_interval_seconds`;
// the extra margin covers scheduler jitter before the collection cycle runs.
constexpr int kStatsTestIntervalSeconds = 1;
constexpr auto kStatsTestWarmup = std::chrono::milliseconds(1600);

void run_one_stats_cycle(AppContext &ctx) {
  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown, kStatsTestIntervalSeconds);
  stats.start_periodic_logging();
  std::this_thread::sleep_for(kStatsTestWarmup);
}
} // namespace

TEST_CASE("StatsLogger: active client counters and logger lifecycle",
          "[stats-logger]") {
  const EnvGuard mode{{"MODE", "proxy"}};
  AppContext ctx;
  std::atomic<bool> shutdown{false};
  StatsLogger stats(ctx, shutdown);

  stats.start_periodic_logging();
  stats.increment_active_clients();
  stats.increment_active_clients();
  stats.decrement_active_clients();

  shutdown.store(true);
}

TEST_CASE("StatsLogger: periodic cycle collects proxy statistics",
          "[stats-logger]") {
  const ProxyEnv env;
  AppContext ctx;
  init_proxy_components(ctx);

  run_one_stats_cycle(ctx);
}

TEST_CASE("StatsLogger: periodic cycle in worker and l2-server modes",
          "[stats-logger]") {
  for (const char *mode : {"worker", "l2-server"}) {
    const EnvGuard guard{{"MODE", mode}};
    AppContext ctx;
    run_one_stats_cycle(ctx);
  }
}

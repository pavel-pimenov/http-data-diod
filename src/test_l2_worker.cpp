// Coverage for the worker half of the coverage target: the outbound L2 call
// pipeline in l2_worker.cpp and the NATS task handlers in l2_worker_nats.cpp.
// Everything runs against a locally constructed AppContext, a loopback
// httplib::Server standing in for the L2 backend and a JaegerLogger pointed at
// a dead port — no NATS broker and no database are involved.
//
// Connecting has to fail FAST, same TLS-CA trick as in
// test_proxy_handlers.cpp: natsConnection_Connect retries forever while the
// server is unreachable, so a CA file that does not exist aborts the connect
// before the client ever dials and every publish/subscribe returns immediately.
//
// The private pipeline stages are driven through L2WorkerTestAccess, which
// l2_worker.hpp declares a friend of L2Worker.

#include "app_context.hpp"
#include "db_query_utils.hpp"
#include "httplib/httplib.h"
#include "json_utils.hpp"
#include "l2_worker.hpp"
#include "retry_handler.hpp"
#include "trace_logger.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern std::atomic<bool> g_shutdown_flag;

namespace {

// W3C traceparent carrying a 32-hex trace id, so the tracer branches of the
// pipeline see a non-empty trace context.
constexpr const char *kTraceparent =
    "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

// RAII override of several env vars; nullopt unsets the variable. The values
// must be in place before the Config is loaded by the AppContext ctor.
class EnvGuard {
public:
  explicit EnvGuard(const std::map<std::string, std::optional<std::string>>
                        &vars) {
    m_vars.reserve(vars.size());
    for (const auto &[name, value] : vars) {
      m_vars.emplace_back(name, read_env(name.c_str()));
      if (value.has_value()) {
        setenv(name.c_str(), value->c_str(), 1);
      } else {
        unsetenv(name.c_str());
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

// Loopback stand-in for the L2 backend: covers the success, retry, binary and
// content-type-less response paths of the worker call pipeline.
struct L2BackendServer {
  httplib::Server m_server;
  int m_port = 0;
  std::thread m_thread;
  std::atomic<int> m_value_posts{0};
  std::atomic<int> m_flaky_hits{0};

  L2BackendServer() {
    m_server.Post("/api/value",
                  [this](const httplib::Request &, httplib::Response &res) {
                    m_value_posts.fetch_add(1);
                    res.set_header("X-L2-Server", "loopback");
                    res.set_content(R"({"server_span_id":"l2-span-1"})",
                                    "application/json");
                  });

    m_server.Get("/api/value", [](const httplib::Request &,
                                  httplib::Response &res) {
      res.set_content(R"({"server_span_id":"l2-span-2"})", "application/json");
    });

    // First call fails with 502, the retry succeeds: exercises the
    // 502-retry branch of execute_l2_call_with_retry.
    m_server.Post("/api/flaky",
                  [this](const httplib::Request &, httplib::Response &res) {
                    if (m_flaky_hits.fetch_add(1) == 0) {
                      res.status = 502;
                      res.set_content(R"({"error":"bad gateway"})",
                                      "application/json");
                    } else {
                      res.set_content(R"({"server_span_id":"l2-span-3"})",
                                      "application/json");
                    }
                  });

    m_server.Post("/api/binary", [](const httplib::Request &,
                                    httplib::Response &res) {
      res.set_content(std::string("\x00\x01\x02", 3),
                      "application/octet-stream");
    });

    m_server.Get("/api/empty", [](const httplib::Request &,
                                  httplib::Response &res) {
      res.status = 200;
      res.body.clear();
    });

    m_port = m_server.bind_to_any_port("127.0.0.1");
    m_thread = std::thread([this] { m_server.listen_after_bind(); });
    m_server.wait_until_ready();
  }

  ~L2BackendServer() {
    m_server.stop();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  std::string api_url() const {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/api";
  }
};

L2BackendServer &g_l2_backend() {
  static L2BackendServer g_server;
  return g_server;
}

// L2_SERVER_URLS as a JSON array: the loopback backend plus a dead address
// used by the connection-failure and circuit-breaker paths.
std::string l2_server_urls() {
  json urls = json::array({g_l2_backend().api_url(),
                           std::string("http://127.0.0.1:1/dead")});
  return urls.dump();
}

using EnvVars = std::map<std::string, std::optional<std::string>>;

// Worker mode plus the fail-fast NATS setup described in the file header.
// THREAD_POOL_TYPE=none makes the ctor take the "force CUSTOM for worker"
// branch.
EnvVars worker_env(const std::string &urls) {
  EnvVars vars;
  vars["MODE"] = "worker";
  vars["LOG_LEVEL"] = "ERROR";
  vars["THREAD_POOL_TYPE"] = "none";
  vars["L2_WORKER_THREADS"] = "2";
  vars["L2_WORKER_QUEUE_SIZE"] = "16";
  vars["HTTP_TIMEOUT_SECONDS"] = "3";
  vars["HTTP_POOL_SIZE"] = "8";
  vars["MAX_RETRIES"] = "2";
  vars["DEDUP_ENABLED"] = "true";
  vars["NATS_HOST"] = "127.0.0.1";
  vars["NATS_PORT"] = "4222";
  vars["NATS_ENABLE_TLS"] = "true";
  vars["NATS_TLS_CA_CERT_FILE"] = "/nonexistent-test-ca.pem";
  vars["NATS_TLS_CERT_FILE"] = std::nullopt;
  vars["NATS_TLS_KEY_FILE"] = std::nullopt;
  vars["NATS_TIMEOUT_MS"] = "1000";
  vars["DB_QUERY_NATS_TIMEOUT_MS"] = "1000";
  vars["L2_SERVER_HOST"] = "127.0.0.1";
  vars["L2_SERVER_PORT"] = "8088";
  vars["L2_SERVER_URLS"] = urls;
  return vars;
}

// Attaches a tracer whose sender thread never flushes during the test (batch
// and interval far above what a single test produces): the spans are enqueued
// and dropped with the context, but every tracer branch of the worker runs.
void attach_tracer(AppContext &ctx) {
  auto &metrics = *ctx.m_tracing_metrics;
  ctx.m_tracer = std::make_unique<JaegerLogger>(
      "http://127.0.0.1:1/api/traces", metrics.m_spans_sent,
      metrics.m_spans_failed, metrics.m_queue_size, metrics.m_last_send_duration,
      metrics.m_send_latency, metrics.m_queue_time,
      /*batch_size=*/10000, /*flush_interval_ms=*/100000, /*sample_rate=*/1.0);
}

json make_worker_request(const std::string &request_id,
                         const std::string &path) {
  json request;
  request[NatsContract::kRequestId] = request_id;
  request[NatsContract::kMethod] = "POST";
  request[NatsContract::kPath] = path;
  request[NatsContract::kBody] = R"({"payload":"test"})";
  request[NatsContract::kClientIp] = "10.9.8.7";
  request[NatsContract::kProxyIp] = "10.9.8.1";
  request[NatsContract::kTraceparent] = kTraceparent;
  request[NatsContract::kProxyTraceparent] = kTraceparent;
  request[NatsContract::kProxySpanId] = "proxy-inlet-span";
  request[NatsContract::kHeaders] = json{{"X-Real-IP", "10.9.8.7"},
                                         {"X-DataHub-Client-Id", "client-42"},
                                         {"User-Agent", "l2-test/1.0"}};
  return request;
}

} // namespace

// White-box accessor declared a friend of L2Worker in l2_worker.hpp. Members
// of the private nested stage types never leave it: everything the assertions
// need comes back as plain json/string/bool.
class L2WorkerTestAccess {
public:
  static void update_queue_size_metric(L2Worker &worker) {
    worker.update_queue_size_metric();
  }

  static void record_bytes_sent(L2Worker &worker, size_t bytes) {
    worker.record_bytes_sent(bytes);
  }

  static void set_l2_server_urls(L2Worker &worker,
                                 std::vector<std::string> urls) {
    worker.m_l2_server_urls = std::move(urls);
  }

  static bool validate_l2_server_access(L2Worker &worker,
                                        const std::string &path,
                                        std::string &selected_url) {
    return worker.validate_l2_server_access(path, selected_url);
  }

  static std::string extract_l2_server_span_id(L2Worker &worker,
                                               const std::string &body) {
    return worker.extract_l2_server_span_id(body);
  }

  static json prepare_response_headers(L2Worker &worker,
                                       const HttpResponse &response) {
    return worker.prepare_response_headers(response);
  }

  static json prepare_response_data(L2Worker &worker,
                                    const httplib::Headers &headers) {
    L2Worker::L2Response l2_response;
    l2_response.m_headers = headers;
    L2Worker::TracingSpans spans;
    const L2Worker::ResponseData data =
        worker.prepare_response_data(l2_response, spans);
    return json{{"content_type", data.m_content_type},
                {"is_binary", data.m_is_binary},
                {"timestamp_us", data.m_timestamp_us}};
  }

  static json create_tracing_spans(L2Worker &worker,
                                   const TraceContext &parent_trace_ctx,
                                   const std::string &op_parent_span_id) {
    const L2Worker::TracingSpans spans =
        worker.create_tracing_spans(parent_trace_ctx, op_parent_span_id);
    return json{{"worker_process_span_id", spans.m_worker_process_span_id},
                {"worker_process_parent_id", spans.m_worker_process_parent_id},
                {"l2_call_span_id", spans.m_l2_call_span_id},
                {"traceparent", spans.m_traceparent_header}};
  }

  static void process_request_from_nats(L2Worker &worker,
                                        const std::string &request_json,
                                        const std::string &reply_to) {
    worker.process_request_from_nats(request_json, reply_to);
  }

  static void process_db_query_from_nats(L2Worker &worker,
                                         const std::string &request_json,
                                         const std::string &reply_to) {
    worker.process_db_query_from_nats(request_json, reply_to);
  }

  static void observe_db_query_outcome(L2Worker &worker,
                                       const json &request_data, int status,
                                       const json &body, uint64_t db_start_us,
                                       uint64_t db_end_us,
                                       const TraceContext &trace_ctx,
                                       const std::string &request_id,
                                       const std::string &consume_span_id) {
    worker.observe_db_query_outcome(request_data, status, body, db_start_us,
                                    db_end_us, trace_ctx, request_id,
                                    consume_span_id);
  }

  static bool subscribe_db_query_subject(L2Worker &worker) {
    return worker.subscribe_db_query_subject();
  }

  static bool subscribe_worker_subject(L2Worker &worker) {
    return worker.subscribe_worker_subject();
  }

  static bool ensure_db_query_subscription(L2Worker &worker,
                                           RetryHandler &backoff) {
    return worker.ensure_db_query_subscription(backoff);
  }

  static void publish_db_gateway_ready_metric(L2Worker &worker) {
    worker.publish_db_gateway_ready_metric();
  }

  static void reset_nats_client(L2Worker &worker) {
    worker.m_clients.m_nats_client.reset();
  }
};

// ============================================================================
// outbound L2 call
// ============================================================================

TEST_CASE("L2Worker: outbound call routing, forwarded headers and span id",
          "[l2-worker]") {
  const EnvGuard env(worker_env(l2_server_urls()));
  AppContext ctx;
  attach_tracer(ctx);
  L2Worker worker(ctx);

  REQUIRE_FALSE(worker.is_nats_connected());
  L2WorkerTestAccess::update_queue_size_metric(worker);
  L2WorkerTestAccess::record_bytes_sent(worker, 42);

  const httplib::Headers forwarded_headers{
      {"X-Real-IP", "10.1.2.3"},
      {"X-DataHub-Client-Id", "client-7"},
      {"User-Agent", "worker-test/1.0"}};

  const HttpResponse post =
      worker.call_l2_server("/api/value", R"({"q":1})", kTraceparent,
                            forwarded_headers, "POST");
  REQUIRE(post.m_status == 200);
  REQUIRE(post.m_body.find("l2-span-1") != std::string::npos);
  REQUIRE(post.m_headers.count("X-L2-Server") == 1);
  REQUIRE(L2WorkerTestAccess::extract_l2_server_span_id(worker, post.m_body) ==
          "l2-span-1");

  const HttpResponse get =
      worker.call_l2_server("/api/value", "", kTraceparent, {}, "GET");
  REQUIRE(get.m_status == 200);
  REQUIRE(get.m_body.find("l2-span-2") != std::string::npos);

  // Path outside every configured L2 base: rejected before any HTTP call.
  const HttpResponse denied = worker.call_l2_server("/admin", "{}");
  REQUIRE(denied.m_status == 403);

  // Unknown path on the loopback backend: 4xx does not trip the breaker.
  const HttpResponse missing =
      worker.call_l2_server("/api/missing", "{}", "", {}, "POST");
  REQUIRE(missing.m_status == 404);

  const json headers_json =
      L2WorkerTestAccess::prepare_response_headers(worker, get);
  REQUIRE(headers_json.is_object());

  REQUIRE(L2WorkerTestAccess::extract_l2_server_span_id(worker, R"({"x":1})")
              .empty());

  std::string selected_url;
  REQUIRE(L2WorkerTestAccess::validate_l2_server_access(
      worker, "/api/value", selected_url));
  REQUIRE(selected_url == g_l2_backend().api_url());
  REQUIRE_FALSE(L2WorkerTestAccess::validate_l2_server_access(
      worker, "/admin", selected_url));

  // Tracing spans with a live tracer: parent id is taken from the caller,
  // the l2 call span id and traceparent are generated.
  TraceContext parent_ctx;
  parent_ctx.m_trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";
  parent_ctx.m_parent_id = "00f067aa0ba902b7";
  const json spans = L2WorkerTestAccess::create_tracing_spans(
      worker, parent_ctx, "consume-span");
  REQUIRE(spans["worker_process_parent_id"] == "consume-span");
  REQUIRE_FALSE(spans["worker_process_span_id"].get<std::string>().empty());
  REQUIRE_FALSE(spans["l2_call_span_id"].get<std::string>().empty());
  REQUIRE(spans["traceparent"].get<std::string>().size() == 55);
}

// ============================================================================
// retry, circuit breaker, failure handling
// ============================================================================

TEST_CASE("L2Worker: retries, circuit breaker and failure handling",
          "[l2-worker]") {
  const EnvGuard env(worker_env(l2_server_urls()));
  AppContext ctx;
  attach_tracer(ctx);

  // The HTTP pool binds a client to the host of its first request and keeps
  // reusing it, so the worker that talks to the dead backend never sees the
  // loopback one and vice versa.
  {
    L2Worker worker(ctx);

    const HttpResponse dead = worker.call_l2_server("/dead/x", "{}");
    REQUIRE(dead.m_status == 500);
    REQUIRE(dead.m_body.find("Failed to call L2 server") != std::string::npos);

    // Four more failures open the breaker; the next call is rejected before
    // any HTTP attempt.
    for (int i = 0; i < 4; ++i) {
      const HttpResponse failed = worker.call_l2_server("/dead/x", "{}");
      REQUIRE(failed.m_status == 500);
    }
    const HttpResponse rejected = worker.call_l2_server("/dead/x", "{}");
    REQUIRE(rejected.m_status == 503);

    // Empty URL list is reported as a misconfiguration, not as an arbitrary
    // address violation.
    L2WorkerTestAccess::set_l2_server_urls(worker, {});
    std::string selected_url;
    REQUIRE_FALSE(L2WorkerTestAccess::validate_l2_server_access(
        worker, "/api/value", selected_url));
    const HttpResponse no_urls = worker.call_l2_server("/api/value", "{}");
    REQUIRE(no_urls.m_status == 403);

    const json no_content_type =
        L2WorkerTestAccess::prepare_response_data(worker, {});
    REQUIRE(no_content_type["content_type"] == "");
    REQUIRE_FALSE(no_content_type["is_binary"].get<bool>());
    REQUIRE(no_content_type["timestamp_us"].get<uint64_t>() > 0);

    const json binary_type = L2WorkerTestAccess::prepare_response_data(
        worker, {{"Content-Type", "application/octet-stream"}});
    REQUIRE(binary_type["is_binary"].get<bool>());
  }

  // 502 on the first attempt is retried and succeeds on the second.
  L2Worker retry_worker(ctx);
  const HttpResponse recovered = retry_worker.call_l2_server("/api/flaky", "{}");
  REQUIRE(recovered.m_status == 200);
  REQUIRE(g_l2_backend().m_flaky_hits == 2);
}

// ============================================================================
// NATS request pipeline
// ============================================================================

TEST_CASE("L2Worker: NATS request pipeline", "[l2-worker]") {
  const EnvGuard env(worker_env(l2_server_urls()));
  AppContext ctx;
  attach_tracer(ctx);
  L2Worker worker(ctx);
  const std::string reply_to = "l2-proxy.test.reply";

  // Malformed JSON: rejected by the parser before validation.
  L2WorkerTestAccess::process_request_from_nats(worker, "{not json", reply_to);

  // Valid JSON, disallowed method: schema validation fails, the validation
  // metrics, the Sentry capture and the 400 reply all run.
  json disallowed_method = make_worker_request("req-method", "/api/value");
  disallowed_method[NatsContract::kMethod] = "PUT";
  L2WorkerTestAccess::process_request_from_nats(worker,
                                                disallowed_method.dump(),
                                                reply_to);

  // Non-string method: nlohmann throws inside the validator, the handler
  // catches it and answers 500.
  json non_string_method = make_worker_request("req-type", "/api/value");
  non_string_method[NatsContract::kMethod] = 42;
  L2WorkerTestAccess::process_request_from_nats(worker,
                                                non_string_method.dump(),
                                                reply_to);

  const double processed_before =
      ctx.m_worker.m_metrics->m_requests_processed.Value();
  const int value_posts_before = g_l2_backend().m_value_posts.load();

  // Full pipeline: parse, metadata extraction, tracing spans, L2 call,
  // response envelope, dedup store, publish attempt.
  const json ok = make_worker_request("req-ok", "/api/value");
  L2WorkerTestAccess::process_request_from_nats(worker, ok.dump(), reply_to);
  REQUIRE(ctx.m_worker.m_metrics->m_requests_processed.Value() ==
          processed_before + 1);
  REQUIRE(g_l2_backend().m_value_posts.load() == value_posts_before + 1);

  // Same request id again: answered from the dedup cache, no second L2 call.
  const double duplicates_before =
      ctx.m_worker.m_metrics->m_duplicate_requests.Value();
  L2WorkerTestAccess::process_request_from_nats(worker, ok.dump(), reply_to);
  REQUIRE(ctx.m_worker.m_metrics->m_duplicate_requests.Value() ==
          duplicates_before + 1);
  REQUIRE(g_l2_backend().m_value_posts.load() == value_posts_before + 1);

  // Binary L2 response: the envelope carries it base64-encoded.
  const json binary = make_worker_request("req-binary", "/api/binary");
  L2WorkerTestAccess::process_request_from_nats(worker, binary.dump(),
                                                reply_to);

  // A response without a Content-Type stays unencoded and text-typed.
  const json empty_type = make_worker_request("req-empty", "/api/empty");
  L2WorkerTestAccess::process_request_from_nats(worker, empty_type.dump(),
                                                reply_to);

  // The worker subscription goes through the shared reply_to validation +
  // enqueue + catch path of subscribe_nats_subject and fails without a broker.
  REQUIRE_FALSE(L2WorkerTestAccess::subscribe_worker_subject(worker));

  // Without a NATS client the publish path reports the missing client and
  // returns instead of retrying.
  L2WorkerTestAccess::reset_nats_client(worker);
  REQUIRE_FALSE(worker.is_nats_connected());
  L2WorkerTestAccess::process_request_from_nats(
      worker, make_worker_request("req-noclient", "/api/value").dump(),
      reply_to);
}

// ============================================================================
// run loop + HTTP DB gateway handlers
// ============================================================================

TEST_CASE("L2Worker: run loop and DB gateway NATS handlers", "[l2-worker]") {
  EnvVars vars = worker_env(l2_server_urls());
  vars["DB_QUERY_ENABLED"] = "true";
  vars["DB_POSTGRES_ENABLED"] = "true";
  vars["DB_POSTGRES_USER"] = "l2test";
  vars["DB_POSTGRES_PASSWORD"] = "l2test";
  vars["DB_POSTGRES_HOST"] = "127.0.0.1";
  vars["DB_POSTGRES_PORT"] = "1";
  const EnvGuard env(vars);
  AppContext ctx;
  attach_tracer(ctx);
  L2Worker worker(ctx);
  const std::string reply_to = "l2-proxy.db.reply";

  // run() owns the metrics ticker and the NATS loop. The helper thread flips
  // the shutdown flag after the ticker's first ~5s sample, which lets the
  // ticker branch and the loop bookkeeping run while the loop itself keeps
  // failing to reach a broker that is not there.
  g_shutdown_flag = false;
  std::thread shutdown_after_sample([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(5400));
    g_shutdown_flag = true;
  });
  worker.run();
  g_shutdown_flag = false;
  shutdown_after_sample.join();

  REQUIRE(ctx.m_worker.m_metrics->m_graceful_shutdown_seconds.Value() > 0.0);

  // The gateway handler exists (created by the loop) but its executor cannot
  // come up: the subscription keeps failing and the readiness gauge reports
  // the configured database as not ready.
  RetryHandler backoff(1, 150);
  REQUIRE_FALSE(L2WorkerTestAccess::ensure_db_query_subscription(worker,
                                                                 backoff));
  REQUIRE(L2WorkerTestAccess::subscribe_db_query_subject(worker));
  L2WorkerTestAccess::publish_db_gateway_ready_metric(worker);

  // Malformed DB request: answered with BAD_REQUEST.
  L2WorkerTestAccess::process_db_query_from_nats(worker, "{oops", reply_to);

  // Well-formed request naming a database that has no executor.
  const json unknown_db{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
                        {DbQueryContract::kRequestId, "db-unknown"},
                        {DbQueryContract::kDb, "pg"},
                        {DbQueryContract::kSql, "select 1"}};
  L2WorkerTestAccess::process_db_query_from_nats(worker, unknown_db.dump(),
                                                 reply_to);

  // Operational failure: Sentry capture, duration histogram and the DB
  // tracing span.
  TraceContext trace_ctx;
  trace_ctx.m_trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";
  L2WorkerTestAccess::observe_db_query_outcome(
      worker, unknown_db, 500,
      make_db_error_body("DB_UNAVAILABLE", "pool exhausted"),
      1000, 2000, trace_ctx, "db-unknown", "consume-span");
}

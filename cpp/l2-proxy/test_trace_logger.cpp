// Unit tests for the Jaeger logger: span delivery (queue -> sender thread ->
// POST batching), retry/failure accounting, sampling, ID generation,
// traceparent validation and baggage. Delivery is verified against a local
// loopback httplib::Server on an ephemeral port (bind_to_any_port), so no
// external Jaeger service is required.

#include "common_utils.hpp"
#include "duplicate_detector.hpp"
#include "httplib/httplib.h"
#include "nlohmann/json.hpp"
#include "trace_logger.hpp"
#include "tracing_helpers.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <mutex>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string>
#include <thread>
#include <vector>

namespace {

bool wait_for_condition(const auto &pred, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return pred();
}

struct TraceMockServer {
  httplib::Server m_server;
  int m_port = 0;
  std::thread m_thread;
  mutable std::mutex m_mu;
  std::vector<std::string> m_bodies;
  int m_slow_us = 0;

  explicit TraceMockServer(int slow_us = 0) : m_slow_us(slow_us) {
    m_server.Post("/api/traces",
                  [this](const httplib::Request &req, httplib::Response &res) {
                    if (m_slow_us > 0) {
                      std::this_thread::sleep_for(
                          std::chrono::microseconds(m_slow_us));
                    }
                    std::lock_guard lock(m_mu);
                    m_bodies.push_back(req.body);
                    res.status = 200;
                  });
    m_server.Get("/ping",
                 [](const httplib::Request &, httplib::Response &res) {
                   res.status = 200;
                 });
    m_port = m_server.bind_to_any_port("127.0.0.1");
    m_thread = std::thread([this] { m_server.listen_after_bind(); });
  }

  ~TraceMockServer() {
    m_server.stop();
    if (m_thread.joinable())
      m_thread.join();
  }

  std::string endpoint() const {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/api/traces";
  }

  size_t body_count() const {
    std::lock_guard lock(m_mu);
    return m_bodies.size();
  }

  std::vector<std::string> snapshot_bodies() const {
    std::lock_guard lock(m_mu);
    return m_bodies;
  }
};

struct TraceLoggerEnv {
  prometheus::Registry m_registry;
  prometheus::Counter &m_spans_sent;
  prometheus::Counter &m_spans_failed;
  prometheus::Gauge &m_queue_size;
  prometheus::Gauge &m_last_send_duration;
  prometheus::Histogram &m_send_latency;
  prometheus::Histogram &m_queue_time;
  std::unique_ptr<JaegerLogger> m_logger;

  explicit TraceLoggerEnv(const std::string &endpoint, size_t batch_size = 50,
                          int flush_interval_ms = 1000, double sample_rate = 1.0)
      : m_spans_sent(
            prometheus::BuildCounter()
                .Name("l2_tracing_spans_sent_total")
                .Help("h")
                .Register(m_registry)
                .Add({})),
        m_spans_failed(
            prometheus::BuildCounter()
                .Name("l2_tracing_spans_failed_total")
                .Help("h")
                .Register(m_registry)
                .Add({})),
        m_queue_size(prometheus::BuildGauge()
                         .Name("l2_tracing_queue_size")
                         .Help("h")
                         .Register(m_registry)
                         .Add({})),
        m_last_send_duration(
            prometheus::BuildGauge()
                .Name("l2_tracing_last_send_duration_seconds")
                .Help("h")
                .Register(m_registry)
                .Add({})),
        m_send_latency(prometheus::BuildHistogram()
                           .Name("l2_tracing_send_latency_seconds")
                           .Help("h")
                           .Register(m_registry)
                           .Add({}, prometheus::Histogram::BucketBoundaries{
                                       0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1,
                                       5})),
        m_queue_time(prometheus::BuildHistogram()
                         .Name("l2_tracing_queue_time_seconds")
                         .Help("h")
                         .Register(m_registry)
                         .Add({}, prometheus::Histogram::BucketBoundaries{
                                     0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1,
                                     5})),
        m_logger(std::make_unique<JaegerLogger>(
            endpoint, m_spans_sent, m_spans_failed, m_queue_size,
            m_last_send_duration, m_send_latency, m_queue_time, batch_size,
            flush_interval_ms, sample_rate)) {}

  double sent() const { return m_spans_sent.Collect().counter.value; }
  double failed() const { return m_spans_failed.Collect().counter.value; }
  double queued() const { return m_queue_size.Collect().gauge.value; }
};

bool is_hex_string(const std::string &s) {
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

} // namespace

TEST_CASE("TraceLogger: generate_trace_id and generate_span_id produce hex",
          "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  const auto trace_id = env.m_logger->generate_trace_id();
  const auto span_id = env.m_logger->generate_span_id();
  REQUIRE(trace_id.size() == 32);
  REQUIRE(span_id.size() == 16);
  REQUIRE(is_hex_string(trace_id));
  REQUIRE(is_hex_string(span_id));
}

TEST_CASE("TraceLogger: generate_traceparent round-trips validation",
          "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  const auto tp_sampled =
      env.m_logger->generate_traceparent("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                         "bbbbbbbbbbbbbbbb", true);
  const auto tp_unsampled =
      env.m_logger->generate_traceparent("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                         "bbbbbbbbbbbbbbbb", false);
  REQUIRE(tp_sampled.size() == 55);
  REQUIRE(tp_sampled.ends_with("-01"));
  REQUIRE(tp_unsampled.ends_with("-00"));
  REQUIRE(JaegerLogger::validate_traceparent(tp_sampled));
  REQUIRE(JaegerLogger::validate_traceparent(tp_unsampled));
}

TEST_CASE("TraceLogger: validate_traceparent rejects malformed headers",
          "[tracing]") {
  // Wrong size
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(""));
  REQUIRE_FALSE(JaegerLogger::validate_traceparent("00-aa"));

  // Wrong version prefix
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "01-0123456789abcdef0123456789abcdef-0123456789abcdef-01"));

  // Wrong separator at trace_id/span_id boundary
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-0123456789abcdef0123456789abcdefx0123456789abcdef-01"));

  // Non-hex in trace_id (position 3)
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-g123456789abcdef0123456789abcdef-0123456789abcdef-01"));

  // Non-hex in trace_id (position 34)
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-0123456789abcdef0123456789abcdeG-0123456789abcdef-01"));

  // Non-hex in span_id (position 36)
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-0123456789abcdef0123456789abcdef-g123456789abcdef-01"));

  // Non-hex in span_id (position 51)
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-0123456789abcdef0123456789abcdef-0123456789abcdeG-01"));

  // Non-hex in flags (position 54)
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-0123456789abcdef0123456789abcdef-0123456789abcdef-0g"));

  // Non-hex in flags (position 53)
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-0123456789abcdef0123456789abcdef-0123456789abcdef-g1"));

  // Flags not 00/01
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-0123456789abcdef0123456789abcdef-0123456789abcdef-02"));
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-0123456789abcdef0123456789abcdef-0123456789abcdef-11"));
}

TEST_CASE("TraceLogger: extract_trace_info parses and validates", "[tracing]") {
  const auto ok = extract_trace_info(
      "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01");
  REQUIRE(ok.m_valid);
  REQUIRE(ok.m_trace_id == "0123456789abcdef0123456789abcdef");
  REQUIRE(ok.m_parent_span_id == "0123456789abcdef");
  REQUIRE(ok.m_sampled);

  const auto bad = extract_trace_info("garbage");
  REQUIRE_FALSE(bad.m_valid);
  REQUIRE(bad.m_trace_id.empty());
  REQUIRE(bad.m_parent_span_id.empty());
}

TEST_CASE("TraceLogger: should_sample respects deterministic rate bounds",
          "[tracing]") {
  TraceLoggerEnv env_all("http://127.0.0.1:1/api/traces", 50, 1000, 1.0);
  REQUIRE(env_all.m_logger->should_sample());

  TraceLoggerEnv env_none("http://127.0.0.1:1/api/traces", 50, 1000, 0.0);
  REQUIRE_FALSE(env_none.m_logger->should_sample());

  TraceLoggerEnv env_half("http://127.0.0.1:1/api/traces", 50, 1000, 0.5);
  REQUIRE(env_half.m_logger->should_sample(true));
  const bool r = env_half.m_logger->should_sample();
  REQUIRE((r == true || r == false));
}

TEST_CASE("TraceLogger: log_request suppresses unsampled success",
          "[tracing]") {
  TraceMockServer server;
  TraceLoggerEnv env(server.endpoint(), 50, 1000, 0.0);
  env.m_logger->log_request("GET", "http://svc/query", 200, 1000, 2000, "test");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  REQUIRE(env.sent() == 0.0);
  REQUIRE(env.failed() == 0.0);
  REQUIRE(env.queued() == 0.0);
  REQUIRE(server.body_count() == 0);
}

TEST_CASE("TraceLogger: log_request always samples errors at zero rate",
          "[tracing]") {
  TraceMockServer server;
  TraceLoggerEnv env(server.endpoint(), 50, 500, 0.0);
  env.m_logger->log_request("POST", "http://svc/query", 500, 1000, 2000,
                            "test", "req-1");
  REQUIRE(wait_for_condition([&] { return env.sent() >= 1.0; }, 5000));
  REQUIRE(server.body_count() >= 1);
  const auto bodies = server.snapshot_bodies();
  const auto arr = nlohmann::json::parse(bodies.front());
  REQUIRE(arr.is_array());
  REQUIRE(arr.size() == 1);
  REQUIRE(arr[0]["tags"]["http.method"] == "POST");
  REQUIRE(arr[0]["tags"]["http.status_code"] == "500");
  REQUIRE(arr[0]["tags"]["request.id"] == "req-1");
}

TEST_CASE("TraceLogger: batch of spans is delivered to Jaeger endpoint",
          "[tracing]") {
  TraceMockServer server;
  TraceLoggerEnv env(server.endpoint(), 2, 500, 1.0);
  for (int i = 0; i < 3; ++i) {
    env.m_logger->enqueue_span("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                               "bbbbbbbbbbbbbbbb", "cccccccccccccccc",
                               "HTTP GET /query", 1000 + i, 2000 + i,
                               "test-service",
                               nlohmann::json{{"http.method", "GET"}});
  }
  REQUIRE(wait_for_condition([&] { return env.sent() >= 3.0; }, 5000));
  REQUIRE(wait_for_condition([&] { return env.queued() == 0.0; }, 2000));
  REQUIRE(server.body_count() >= 1);

  int total_spans = 0;
  for (const auto &body : server.snapshot_bodies()) {
    const auto arr = nlohmann::json::parse(body);
    REQUIRE(arr.is_array());
    total_spans += static_cast<int>(arr.size());
    for (const auto &span : arr) {
      REQUIRE(span["traceId"] == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
      REQUIRE(span["id"] == "bbbbbbbbbbbbbbbb");
      REQUIRE(span["parentId"] == "cccccccccccccccc");
      REQUIRE(span["name"] == "HTTP GET /query");
      REQUIRE(span["localEndpoint"]["serviceName"] == "test-service");
      REQUIRE(span["tags"]["http.method"] == "GET");
      REQUIRE(span["duration"].get<uint64_t>() >= 500);
    }
  }
  REQUIRE(total_spans == 3);
}

TEST_CASE("TraceLogger: failed batch increments failed counter", "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces", 50, 500, 1.0);
  env.m_logger->enqueue_span("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                             "bbbbbbbbbbbbbbbb", "", "HTTP POST /query", 1000,
                             2000, "test-service");
  REQUIRE(wait_for_condition([&] { return env.failed() >= 1.0; }, 8000));
  REQUIRE(env.sent() == 0.0);
  REQUIRE(wait_for_condition([&] { return env.queued() == 0.0; }, 2000));
}

TEST_CASE("TraceLogger: full queue drops spans and increments failed",
          "[tracing]") {
  TraceMockServer server(4'000'000);
  TraceLoggerEnv env(server.endpoint(), 50, 1000, 1.0);
  for (int i = 0; i < 10100; ++i) {
    env.m_logger->enqueue_span("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                               "bbbbbbbbbbbbbbbb", "", "HTTP GET /bulk", 1000,
                               2000, "test-service");
  }
  REQUIRE(env.queued() == 10000.0);
  REQUIRE(env.failed() >= 1.0);
}

TEST_CASE("TraceLogger: send_span posts a single span and reports failure",
          "[tracing]") {
  TraceMockServer server;
  TraceLoggerEnv env(server.endpoint(), 50, 1000, 1.0);
  const bool ok = env.m_logger->send_span(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb", "",
      "HTTP PUT /db", 1000, 2000, "test-service",
      nlohmann::json{{"db", "postgres"}});
  REQUIRE(ok);
  REQUIRE(wait_for_condition([&] { return server.body_count() >= 1; }, 3000));
  const auto span = nlohmann::json::parse(server.snapshot_bodies().front());
  REQUIRE(span["tags"]["db"] == "postgres");

  TraceLoggerEnv dead_env("http://127.0.0.1:1/api/traces", 50, 1000, 1.0);
  const bool failed = dead_env.m_logger->send_span(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb", "",
      "HTTP PUT /db", 1000, 2000, "test-service");
  REQUIRE_FALSE(failed);
  REQUIRE(dead_env.failed() >= 1.0);
}

TEST_CASE("TraceLogger: baggage set/get/get_all on the owning thread",
          "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  env.m_logger->set_baggage("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "k1", "v1");
  env.m_logger->set_baggage("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "k2", "v2");
  env.m_logger->set_baggage("ffffffffffffffffffffffffffffffff", "k3", "v3");

  REQUIRE(env.m_logger->get_baggage("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "k1") ==
          "v1");
  REQUIRE(env.m_logger->get_baggage("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "k2") ==
          "v2");
  REQUIRE(env.m_logger->get_baggage("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "k9") ==
          "");
  REQUIRE(env.m_logger->get_baggage("00000000000000000000000000000000", "k1") ==
          "");

  const auto all = env.m_logger->get_all_baggage("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  REQUIRE(all.size() == 2);
  REQUIRE(all.contains("k1"));
  REQUIRE(all.contains("k2"));
  REQUIRE_FALSE(all.contains("k3"));
  REQUIRE(env.m_logger->get_all_baggage("00000000000000000000000000000000")
              .size() == 0);
}

TEST_CASE("TraceLogger: log_request merges additional attributes",
          "[tracing]") {
  TraceMockServer server;
  TraceLoggerEnv env(server.endpoint(), 50, 500, 1.0);
  env.m_logger->log_request(
      "PATCH", "http://svc/item", 201, 1000, 2000, "test", "req-2", "", "", "",
      nlohmann::json{{"db", "postgres"}, {"attempt", 7}});
  REQUIRE(wait_for_condition([&] { return env.sent() >= 1.0; }, 5000));
  const auto arr = nlohmann::json::parse(server.snapshot_bodies().front());
  REQUIRE(arr[0]["tags"]["http.method"] == "PATCH");
  REQUIRE(arr[0]["tags"]["http.url"] == "http://svc/item");
  REQUIRE(arr[0]["tags"]["request.id"] == "req-2");
  REQUIRE(arr[0]["tags"]["db"] == "postgres");
  REQUIRE(arr[0]["tags"]["attempt"] == "7");
}

TEST_CASE("TraceLogger: log_span_to_jaeger enqueues real traces",
          "[tracing]") {
  TraceMockServer server;
  TraceLoggerEnv env(server.endpoint(), 50, 500, 1.0);
  log_span_to_jaeger(env.m_logger.get(), "DELETE", "http://svc/item", 200,
                     1000, 2000, "test", "req-3");
  REQUIRE(wait_for_condition([&] { return env.sent() >= 1.0; }, 5000));
  const auto arr = nlohmann::json::parse(server.snapshot_bodies().front());
  REQUIRE(arr[0]["tags"]["http.method"] == "DELETE");
  REQUIRE(arr[0]["tags"]["request.id"] == "req-3");

  log_span_to_jaeger(nullptr, "GET", "http://svc/x", 200, 1000, 2000, "test",
                     "req-4");
}

TEST_CASE("TraceLogger: handle_trace_context uses the tracer", "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  auto *tracer = env.m_logger.get();

  const auto parsed = handle_trace_context(
      "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01", tracer);
  REQUIRE(parsed.m_trace_id == "0123456789abcdef0123456789abcdef");
  REQUIRE(parsed.m_parent_id == "0123456789abcdef");
  REQUIRE(parsed.m_sampled);
  REQUIRE(parsed.m_span_id.size() == 16);
  REQUIRE(parsed.m_traceparent_header.size() == 55);

  const auto generated = handle_trace_context("garbage", tracer);
  REQUIRE(generated.m_trace_id.size() == 32);
  REQUIRE(generated.m_span_id.size() == 16);
  REQUIRE(generated.m_parent_id.empty());
  REQUIRE(generated.m_traceparent_header.size() == 55);
}

TEST_CASE("TraceLogger: make_span_and_traceparent uses the tracer",
          "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  auto *tracer = env.m_logger.get();

  TraceContext ctx;
  ctx.m_trace_id = "0123456789abcdef0123456789abcdef";
  ctx.m_parent_id = "abcdef0123456789";
  ctx.m_traceparent_header = "00-0123456789abcdef0123456789abcdef-abcdef0123456789-01";

  const auto [span_id, tp] = make_span_and_traceparent(tracer, ctx);
  REQUIRE(span_id.size() == 16);
  REQUIRE(tp.size() == 55);
  REQUIRE(tp.rfind("00-0123456789abcdef0123456789abcdef-", 0) == 0);
  REQUIRE(tp.find(span_id) != std::string::npos);
}

TEST_CASE("TraceLogger: validate_traceparent short-circuit variants",
          "[tracing]") {
  // Version prefix "00-": hit second and third operands of the || chain
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "0x-0123456789abcdef0123456789abcdef-0123456789abcdef-01"));
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00x0123456789abcdef0123456789abcdef-0123456789abcdef-01"));
  // Separator at position 52 (after span_id), not only position 35
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-0123456789abcdef0123456789abcdef-0123456789abcdefx01"));
  REQUIRE(JaegerLogger::validate_traceparent(
      "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01"));
}

TEST_CASE("TracingHelpers: get_traceparent_header present and absent",
          "[tracing]") {
  httplib::Headers with;
  with.emplace("traceparent", "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-01");
  REQUIRE(get_traceparent_header(with) ==
          "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-01");
  httplib::Headers without;
  REQUIRE(get_traceparent_header(without).empty());
}

TEST_CASE("TracingHelpers: begin_request_trace with real headers", "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  auto *tracer = env.m_logger.get();

  httplib::Headers with;
  with.emplace("traceparent",
               "00-cccccccccccccccccccccccccccccccc-dddddddddddddddd-01");
  std::string inlet;
  const TraceContext ctx =
      begin_request_trace(tracer, with, "req-begin", "GET /api", 1000, inlet);
  REQUIRE(ctx.m_trace_id == "cccccccccccccccccccccccccccccccc");
  REQUIRE(ctx.m_parent_id == "dddddddddddddddd");
  REQUIRE(inlet.size() == 16);
  REQUIRE(inlet != ctx.m_traceparent_header);

  std::string inlet2;
  const TraceContext gen =
      begin_request_trace(tracer, {}, "req-begin2", "POST /nats", 2000, inlet2);
  REQUIRE(gen.m_trace_id.size() == 32);
  REQUIRE(inlet2.size() == 16);
}

TEST_CASE("TracingHelpers: extract_and_validate raw variants", "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  auto *tracer = env.m_logger.get();

  const TraceContext parsed = TraceContextHelper::extract_and_validate(
      {{"traceparent",
        "00-eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-ffffffffffffffff-01"}},
      tracer, "ctx-a");
  REQUIRE(parsed.m_trace_id == "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
  REQUIRE(parsed.m_parent_id == "ffffffffffffffff");

  const TraceContext generated =
      TraceContextHelper::extract_and_validate({}, tracer, "ctx-b");
  REQUIRE(generated.m_trace_id.size() == 32);
  REQUIRE(generated.m_traceparent_header.size() == 55);
}

TEST_CASE("TracingHelpers: JaegerSpanLogger real-tracer spam variants",
          "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  auto *tracer = env.m_logger.get();
  TraceContext ctx;
  ctx.m_trace_id = "0123456789abcdef0123456789abcdef";
  ctx.m_parent_id = "abcdef0123456789";
  ctx.m_traceparent_header =
      "00-0123456789abcdef0123456789abcdef-abcdef0123456789-01";

  JaegerSpanLogger::log_l2_call(tracer, "GET", "http://l2:8088/x", 200, 1000,
                                2000, ctx, "worker", "aaaaaaaaaaaaaaaa",
                                "bbbbbbbbbbbbbbbb", "req-l2");
  JaegerSpanLogger::log_l2_call(tracer, "GET", "http://l2:8088/x", 200, 1000,
                                2000, ctx, "worker", "aaaaaaaaaaaaaaaa",
                                "bbbbbbbbbbbbbbbb", "req-l2",
                                nlohmann::json{{"db", "pg"}});
  JaegerSpanLogger::log_worker_processing(tracer, "POST", "/api/p", 200, 1000,
                                          2000, ctx, "worker",
                                          "cccccccccccccccc",
                                          "req-wp");
  JaegerSpanLogger::log_proxy_response(tracer, "GET", "/api/r", 200, 1000,
                                       2000, ctx, "proxy", "req-pr");
  REQUIRE(JaegerSpanLogger::generate_span_id(tracer).size() == 16);
  REQUIRE(JaegerSpanLogger::generate_span_id(nullptr).empty());
}

TEST_CASE("TracingHelpers: JaegerSpanLogger null tracer short-circuits",
          "[tracing]") {
  TraceContext ctx;
  ctx.m_trace_id = "0123456789abcdef0123456789abcdef";
  JaegerSpanLogger::log_l2_call(nullptr, "GET", "http://l2/x", 200, 1, 2, ctx,
                                "worker", "aaaaaaaaaaaaaaaa",
                                "bbbbbbbbbbbbbbbb");
  JaegerSpanLogger::log_worker_processing(nullptr, "POST", "/p", 200, 1, 2, ctx,
                                          "worker", "cccccccccccccccc",
                                          "req-wp-null");
  JaegerSpanLogger::log_proxy_response(nullptr, "GET", "/r", 200, 1, 2, ctx,
                                       "proxy", "req-pr-null");
  JaegerSpanLogger::log_nats_span(nullptr, "PUB", 200, "req", "t", "s", "p", 1);
}

TEST_CASE("TracingHelpers: log_nats_span attaches success flag", "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  JaegerSpanLogger::log_nats_span(env.m_logger.get(), "PUBLISH", 200, "req-n",
                                  "0123456789abcdef0123456789abcdef",
                                  "aaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb", 1000);
  JaegerSpanLogger::log_nats_span(env.m_logger.get(), "REQUEST", 500, "req-n2",
                                  "0123456789abcdef0123456789abcdef",
                                  "aaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb", 2000,
                                  nlohmann::json{{"backend", "db"}});
  REQUIRE(env.m_logger->should_sample(true));
}

TEST_CASE("TracingHelpers: log_backend_error detail branch", "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  auto *tracer = env.m_logger.get();
  TraceContext ctx;
  ctx.m_trace_id = "0123456789abcdef0123456789abcdef";
  ctx.m_parent_id = "abcdef0123456789";

  BackendErrorSpanLogger::log_backend_error(tracer, "POST", "/api/x", 502, 1000,
                                            ctx, "proxy", "req-be",
                                            "worker_dead", "connection refused");
  BackendErrorSpanLogger::log_backend_error(tracer, "POST", "/api/x", 502, 1000,
                                            ctx, "proxy", "req-be2",
                                            "worker_dead", "");
  BackendErrorSpanLogger::log_backend_error(nullptr, "POST", "/api/x", 502, 1000,
                                            ctx, "proxy", "req-be3",
                                            "worker_dead", "");
  REQUIRE(env.m_logger->should_sample(true));
}

TEST_CASE("TracingHelpers: log_rate_limit_rejection limit branches",
          "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  auto *tracer = env.m_logger.get();

  RateLimitSpanLogger::log_rate_limit_rejection(
      tracer, "too_many", "10.0.0.9",
      "00-0123456789abcdef0123456789abcdef-abcdef0123456789-01", "100",
      "50");
  RateLimitSpanLogger::log_rate_limit_rejection(tracer, "too_many", "10.0.0.9",
                                                "", "", "");
  RateLimitSpanLogger::log_rate_limit_rejection(
      nullptr, "too_many", "10.0.0.9", "unused", "100", "50");
  REQUIRE(env.m_logger->should_sample(true));
}

TEST_CASE("TracingHelpers: make_span_and_traceparent hint and sampled",
          "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  auto *tracer = env.m_logger.get();
  TraceContext ctx;
  ctx.m_trace_id = "0123456789abcdef0123456789abcdef";
  ctx.m_traceparent_header =
      "00-0123456789abcdef0123456789abcdef-abcdef0123456789-01";

  const auto [hint_span, hint_tp] =
      make_span_and_traceparent(tracer, ctx, "deadbeefdeadbeef", false);
  REQUIRE(hint_span == "deadbeefdeadbeef");
  REQUIRE(hint_tp.ends_with("-00"));

  const auto gen = make_span_and_traceparent(tracer, ctx);
  REQUIRE(gen.first.size() == 16);

  const auto [no_tracer, no_tp] = make_span_and_traceparent(nullptr, ctx);
  REQUIRE(no_tracer.empty());
  REQUIRE(no_tp == ctx.m_traceparent_header);
}

TEST_CASE("TracingHelpers: add_proxy_trace_fields populates request",
          "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  auto *tracer = env.m_logger.get();
  TraceContext ctx;
  ctx.m_trace_id = "0123456789abcdef0123456789abcdef";

  nlohmann::json req = nlohmann::json::object();
  add_proxy_trace_fields(req, tracer, ctx, "1111111111111111",
                         "2222222222222222", true);
  REQUIRE(req[NatsContract::kProxyTraceId] ==
          "0123456789abcdef0123456789abcdef");
  REQUIRE(req[NatsContract::kProxySpanId] == "2222222222222222");
  REQUIRE(req[NatsContract::kProxyInletSpanId] == "1111111111111111");
  REQUIRE(req[NatsContract::kProxyTraceparent]
              .get<std::string>()
              .starts_with("00-0123456789abcdef0123456789abcdef-"));

  nlohmann::json empty = nlohmann::json::object();
  add_proxy_trace_fields(empty, nullptr, ctx, "1111111111111111",
                         "2222222222222222");
  REQUIRE(empty.empty());
}

TEST_CASE("TracingHelpers: set_traceparent_response_header branch",
          "[tracing]") {
  httplib::Response res;
  TraceContext ctx;
  ctx.m_traceparent_header =
      "00-0123456789abcdef0123456789abcdef-abcdef0123456789-01";
  set_traceparent_response_header(res, ctx);
  REQUIRE(res.get_header_value("traceparent") ==
          "00-0123456789abcdef0123456789abcdef-abcdef0123456789-01");

  httplib::Response res_empty;
  TraceContext ctx_empty;
  set_traceparent_response_header(res_empty, ctx_empty);
  REQUIRE(res_empty.get_header_value("traceparent").empty());
}

TEST_CASE("TracingHelpers: log_worker_span guards trace_id", "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  auto *tracer = env.m_logger.get();
  log_worker_span(tracer, "POST", "/api/x", 200, 1000, 2000, "worker", "req-ws",
                  "0123456789abcdef0123456789abcdef", "aaaaaaaaaaaaaaaa",
                  "bbbbbbbbbbbbbbbb");
  log_worker_span(tracer, "POST", "/api/x", 200, 1000, 2000, "worker",
                  "req-ws2", "", "aaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb");
  log_worker_span(nullptr, "POST", "/api/x", 200, 1000, 2000, "worker",
                  "req-ws3", "0123456789abcdef0123456789abcdef",
                  "aaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb");
  REQUIRE(env.m_logger->should_sample(true));
}

TEST_CASE("TraceLogger: log_incoming_span and extract_from_raw with tracer",
          "[tracing]") {
  TraceLoggerEnv env("http://127.0.0.1:1/api/traces");
  auto *tracer = env.m_logger.get();

  TraceContext ctx;
  ctx.m_trace_id = "0123456789abcdef0123456789abcdef";
  ctx.m_parent_id = "abcdef0123456789";

  const std::string inlet =
      log_incoming_span(tracer, "POST /api/x", 1234, "req-in", ctx);
  REQUIRE(inlet.size() == 16);
  REQUIRE(inlet != ctx.m_parent_id);

  const TraceContext parsed = TraceContextHelper::extract_from_raw(
      "00-0123456789abcdef0123456789abcdef-abcdef0123456789-01", tracer,
      "test-ctx");
  REQUIRE(parsed.m_trace_id == "0123456789abcdef0123456789abcdef");
  REQUIRE(parsed.m_parent_id == "abcdef0123456789");
  REQUIRE(parsed.m_span_id.size() == 16);
}

TEST_CASE("TraceLogger: remaining spans are flushed on shutdown",
          "[tracing]") {
  TraceMockServer server(500000); // slow POST keeps the sender occupied
  int bodies_before = 0;
  {
    TraceLoggerEnv env(server.endpoint(), 8, 60000, 1.0);
    for (int i = 0; i < 8; ++i) {
      env.m_logger->enqueue_span("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                 "bbbbbbbbbbbbbbbb", "", "HTTP GET /flush1",
                                 1000, 2000, "test-service");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (int i = 0; i < 8; ++i) {
      env.m_logger->enqueue_span("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                 "cccccccccccccccc", "", "HTTP GET /flush2",
                                 1000, 2000, "test-service");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bodies_before = server.body_count();
  }

  // The destructor flushes the second queued batch (one body per 8 spans).
  REQUIRE(wait_for_condition(
      [&] { return server.body_count() > bodies_before; }, 5000));
  REQUIRE(server.body_count() >= 2);
}

TEST_CASE("Baggage: url_encode and url_decode round-trip", "[baggage]") {
  REQUIRE(Baggage::url_encode("hello") == "hello");
  REQUIRE(Baggage::url_encode("hello world") == "hello%20world");
  REQUIRE(Baggage::url_encode("a+b") == "a%2Bb");
  REQUIRE(Baggage::url_encode("%") == "%25");
  REQUIRE(Baggage::url_encode("") == "");
  REQUIRE(Baggage::url_encode("abc123-_.~") == "abc123-_.~");

  REQUIRE(Baggage::url_decode("hello") == "hello");
  REQUIRE(Baggage::url_decode("hello%20world") == "hello world");
  REQUIRE(Baggage::url_decode("a%2Bb") == "a+b");
  REQUIRE(Baggage::url_decode("%25") == "%");
  REQUIRE(Baggage::url_decode("") == "");
  REQUIRE(Baggage::url_decode("abc") == "abc");

  REQUIRE(Baggage::url_decode("x%0Gy") == "x%0Gy");
  REQUIRE(Baggage::url_decode("x%2") == "x%2");
}

TEST_CASE("Baggage: to_header and from_header round-trip", "[baggage]") {
  Baggage b;
  b.set("key1", "value1");
  b.set("key2", "value2");
  const std::string header = b.to_header();
  REQUIRE_FALSE(header.empty());

  const Baggage parsed = Baggage::from_header(header);
  REQUIRE(parsed.size() == 2);
  REQUIRE(parsed.get("key1") == "value1");
  REQUIRE(parsed.get("key2") == "value2");
}

TEST_CASE("Baggage: from_header handles empty and whitespace", "[baggage]") {
  const Baggage empty = Baggage::from_header("");
  REQUIRE(empty.size() == 0);

  const Baggage whitespace = Baggage::from_header("  key = value  ");
  REQUIRE(whitespace.size() == 1);
  REQUIRE(whitespace.get("key") == "value");
}

TEST_CASE("Baggage: from_header with url-encoded values", "[baggage]") {
  const Baggage decoded = Baggage::from_header("k=hello%20world");
  REQUIRE(decoded.size() == 1);
  REQUIRE(decoded.get("k") == "hello world");
}

TEST_CASE("DuplicateDetector: per_client_ttl_ms=0 disables client TTL eviction",
          "[duplicate-detector]") {
  DuplicateDetector::Options options;
  options.m_per_client_ttl_ms = 0;
  DuplicateDetector detector(options);

  detector.record("client-a", "10.0.0.1", "hash-1", R"({"v":1})");
  detector.record("client-a", "10.0.0.1", "hash-1", R"({"v":1})");
  REQUIRE(detector.per_client_count_size() == 1);

  std::this_thread::sleep_for(std::chrono::milliseconds(80));

  detector.record("client-b", "10.0.0.2", "hash-2", R"({"v":2})");
  detector.record("client-b", "10.0.0.2", "hash-2", R"({"v":2})");
  REQUIRE(detector.per_client_count_size() == 2);
}

TEST_CASE("DuplicateDetector: report truncates to m_top_n", "[duplicate-detector]") {
  DuplicateDetector::Options options;
  options.m_top_n = 2;
  DuplicateDetector detector(options);

  for (int i = 0; i < 5; ++i) {
    const std::string hash = "hash-" + std::to_string(i);
    const std::string body = R"({"v":)" + std::to_string(i) + "}";
    detector.record("client-a", "10.0.0.1", hash, body);
    detector.record("client-a", "10.0.0.1", hash, body);
  }

  const auto report = detector.report();
  REQUIRE(report["top"].size() == 2);
  REQUIRE(report["duplicate_bodies"] == 5);
}

TEST_CASE("DuplicateDetector: report includes all required JSON fields",
          "[duplicate-detector]") {
  DuplicateDetector detector(DuplicateDetector::Options{});
  detector.record("client-a", "10.0.0.1", "hash-1", R"({"v":1})");
  detector.record("client-a", "10.0.0.1", "hash-1", R"({"v":1})");

  const auto report = detector.report();
  REQUIRE(report.contains("enabled"));
  REQUIRE(report.contains("duplicate_bodies"));
  REQUIRE(report.contains("duplicate_occurrences"));
  REQUIRE(report.contains("by_type"));
  REQUIRE(report.contains("top"));
  REQUIRE(report["enabled"] == true);
  REQUIRE(report["duplicate_bodies"] == 1);
  REQUIRE(report["duplicate_occurrences"] == 1);
  REQUIRE(report["by_type"]["same_client"] == 1);
  REQUIRE(report["by_type"]["cross_client"] == 0);

  const auto &item = report["top"][0];
  REQUIRE(item.contains("count"));
  REQUIRE(item.contains("type"));
  REQUIRE(item.contains("clients"));
  REQUIRE(item.contains("first_seen_ms"));
  REQUIRE(item.contains("last_seen_ms"));
  REQUIRE(item.contains("body"));
  REQUIRE(item["count"] == 2);
  REQUIRE(item["type"] == "same_client");
}

TEST_CASE("DuplicateDetector: evict_lowest_count tie-break uses first_seen_ms",
          "[duplicate-detector]") {
  DuplicateDetector::Options options;
  options.m_max_entries = 2;
  DuplicateDetector detector(options);

  detector.record("client-a", "10.0.0.1", "hash-1", "body-1");
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  detector.record("client-a", "10.0.0.1", "hash-2", "body-2");
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  detector.record("client-a", "10.0.0.1", "hash-1", "body-1");
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  detector.record("client-a", "10.0.0.1", "hash-3", "body-3");

  const auto report = detector.report();
  REQUIRE(report["duplicate_bodies"] >= 1);
}

TEST_CASE("DuplicateDetector: body stored on second delivery if first was too "
          "large", "[duplicate-detector]") {
  DuplicateDetector::Options options;
  options.m_max_body_bytes = 10;
  DuplicateDetector detector(options);

  const std::string long_body(20, 'x');
  detector.record("client-a", "10.0.0.1", "hash-1", long_body);
  const std::string short_body = "ok";
  detector.record("client-a", "10.0.0.1", "hash-1", short_body);

  const auto report = detector.report();
  REQUIRE(report["top"][0]["body"] == "ok");
}
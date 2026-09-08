// Unit tests for the Jaeger logger: span delivery (queue -> sender thread ->
// POST batching), retry/failure accounting, sampling, ID generation,
// traceparent validation and bagga. Delivery is verified against a local
// loopback httplib::Server on an ephemeral port (bind_to_any_port), so no
// external Jaeger service is required.

#include "httplib/httplib.h"
#include "nlohmann/json.hpp"
#include "trace_logger.hpp"

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
  prometheus::Registry registry;
  prometheus::Counter &spans_sent;
  prometheus::Counter &spans_failed;
  prometheus::Gauge &queue_size;
  prometheus::Gauge &last_send_duration;
  prometheus::Histogram &send_latency;
  prometheus::Histogram &queue_time;
  std::unique_ptr<JaegerLogger> m_logger;

  TraceLoggerEnv(const std::string &endpoint, size_t batch_size = 50,
                 int flush_interval_ms = 1000, double sample_rate = 1.0)
      : spans_sent(
            prometheus::BuildCounter()
                .Name("l2_tracing_spans_sent_total")
                .Help("h")
                .Register(registry)
                .Add({})),
        spans_failed(
            prometheus::BuildCounter()
                .Name("l2_tracing_spans_failed_total")
                .Help("h")
                .Register(registry)
                .Add({})),
        queue_size(prometheus::BuildGauge()
                       .Name("l2_tracing_queue_size")
                       .Help("h")
                       .Register(registry)
                       .Add({})),
        last_send_duration(
            prometheus::BuildGauge()
                .Name("l2_tracing_last_send_duration_seconds")
                .Help("h")
                .Register(registry)
                .Add({})),
        send_latency(prometheus::BuildHistogram()
                         .Name("l2_tracing_send_latency_seconds")
                         .Help("h")
                         .Register(registry)
                         .Add({}, prometheus::Histogram::BucketBoundaries{
                                     0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1,
                                     5})),
        queue_time(prometheus::BuildHistogram()
                       .Name("l2_tracing_queue_time_seconds")
                       .Help("h")
                       .Register(registry)
                       .Add({}, prometheus::Histogram::BucketBoundaries{
                                   0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1,
                                   5})),
        m_logger(std::make_unique<JaegerLogger>(
            endpoint, spans_sent, spans_failed, queue_size, last_send_duration,
            send_latency, queue_time, batch_size, flush_interval_ms,
            sample_rate)) {}

  double sent() const { return spans_sent.Collect().counter.value; }
  double failed() const { return spans_failed.Collect().counter.value; }
  double queued() const { return queue_size.Collect().gauge.value; }
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
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(""));
  REQUIRE_FALSE(JaegerLogger::validate_traceparent("00-aa"));
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "01-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-01"));
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaZbbbbbbbbbbbbbbb-01"));
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-02"));
  REQUIRE_FALSE(JaegerLogger::validate_traceparent(
      "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-11"));
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
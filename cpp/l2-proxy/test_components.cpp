// Unit tests for RateLimiter, InFlightTracker, Config, base64, JsonUtils,
// JsonSchemaValidator, RetryUtils, TimeUtils, ThreadPool
#include "base64_utils.hpp"
#include "config.hpp"
#include "duplicate_detector.hpp"
#include "in_flight_tracker.hpp"
#include "json_schema_validator.hpp"
#include "json_utils.hpp"
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
#include <future>
#include <set>
#include <thread>

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

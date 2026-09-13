// Unit tests for the HTTP pipeline: HttpClient (cpp-httplib wrapper) verified
// against a local loopback httplib::Server, and HttpClientPool (connection
// acquisition, reuse, release, idle eviction and Prometheus metrics wiring).

#include "http_client.hpp"
#include "http_client_pool.hpp"
#include "httplib/httplib.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string>
#include <thread>

namespace {

// Local loopback server shared by the HttpClient round-trip tests. Runs on an
// ephemeral port (bind_to_any_port) so no external service is required.
struct TestHttpServer {
  httplib::Server m_server;
  int m_port = 0;
  std::thread m_thread;
  std::atomic<int> m_echo_posts{0};

  TestHttpServer() {
    m_server.Get("/json", [](const httplib::Request &, httplib::Response &res) {
      res.set_content(R"({"hello":"world"})", "application/json");
    });

    m_server.Get("/not-found",
                 [](const httplib::Request &, httplib::Response &res) {
                   res.status = 404;
                   res.set_content(R"({"error":"not found"})",
                                   "application/json");
                 });

    m_server.Post("/echo",
                  [this](const httplib::Request &req, httplib::Response &res) {
                    m_echo_posts.fetch_add(1);
                    res.set_header("Echo-Content-Type",
                                   req.get_header_value("Content-Type"));
                    res.set_header("Echo-Content-Length",
                                   req.get_header_value("Content-Length"));
                    res.set_header("Echo-Traceparent",
                                   req.get_header_value("traceparent"));
                    res.set_header("Echo-Custom",
                                   req.get_header_value("X-Custom-Header"));
                    res.set_content(req.body, "application/json");
                  });

    m_server.Post("/created",
                  [](const httplib::Request &, httplib::Response &res) {
                    res.status = 201;
                    res.set_header("X-Resource-Id", "42");
                    res.set_content(R"({"created":true})", "application/json");
                  });

    m_port = m_server.bind_to_any_port("127.0.0.1");
    m_thread = std::thread([this] { m_server.listen_after_bind(); });
    m_server.wait_until_ready();
  }

  ~TestHttpServer() {
    m_server.stop();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  std::string base_url() const {
    return "http://127.0.0.1:" + std::to_string(m_port);
  }
};

TestHttpServer &g_test_server() {
  static TestHttpServer g_server;
  return g_server;
}

} // namespace

// ---------------------------------------------------------------------------
// HttpClient
// ---------------------------------------------------------------------------

TEST_CASE("HttpClient: POST round-trip returns status, body and code",
          "[http-client]") {
  HttpClient client(5);
  const std::string url = g_test_server().base_url() + "/echo";
  const std::string body = R"({"msg":"ping"})";
  const auto response = client.post(url, body);
  REQUIRE(response.m_status == 200);
  REQUIRE(response.m_body == body);
  REQUIRE(client.get_last_status_code() == 200);
}

TEST_CASE("HttpClient: POST forwards content-length, content-type, "
          "traceparent and custom headers", "[http-client]") {
  HttpClient client(5);
  const std::string url = g_test_server().base_url() + "/echo";
  const std::string body = R"({"msg":"ping"})";
  const auto response =
      client.post(url, body, "00-1111222233334444",
                  {{"X-Custom-Header", "custom-value"}});
  const auto header_value = [&response](const std::string &key) {
    const auto it = response.m_headers.find(key);
    return it == response.m_headers.end() ? std::string{} : it->second;
  };
  REQUIRE(response.m_status == 200);
  REQUIRE(header_value("Echo-Content-Length") == std::to_string(body.size()));
  REQUIRE(header_value("Echo-Content-Type") == "application/json");
  REQUIRE(header_value("Echo-Traceparent") == "00-1111222233334444");
  REQUIRE(header_value("Echo-Custom") == "custom-value");
}

TEST_CASE("HttpClient: GET returns status and JSON body", "[http-client]") {
  HttpClient client(5);
  const auto response = client.get(g_test_server().base_url() + "/json");
  REQUIRE(response.m_status == 200);
  REQUIRE(response.m_body == R"({"hello":"world"})");
  REQUIRE(client.get_last_status_code() == 200);
}

TEST_CASE("HttpClient: non-200 status is reported, not thrown",
          "[http-client]") {
  HttpClient client(5);
  const auto response =
      client.get(g_test_server().base_url() + "/not-found");
  REQUIRE(response.m_status == 404);
  REQUIRE(client.get_last_status_code() == 404);
}

TEST_CASE("HttpClient: custom response headers are exposed", "[http-client]") {
  HttpClient client(5);
  const auto response =
      client.post(g_test_server().base_url() + "/created", R"({})");
  REQUIRE(response.m_status == 201);
  const auto header = response.m_headers.find("X-Resource-Id");
  REQUIRE(header != response.m_headers.end());
  CHECK(header->second == "42");
}

TEST_CASE("HttpClient: post_no_response delivers the body", "[http-client]") {
  HttpClient client(5);
  const int before = g_test_server().m_echo_posts.load();
  client.post_no_response(g_test_server().base_url() + "/echo",
                          R"({"fire":"forget"})");
  REQUIRE(g_test_server().m_echo_posts.load() == before + 1);
}

TEST_CASE("HttpClient: invalid URLs are rejected", "[http-client]") {
  HttpClient client(5);
  auto acquire = [&client](const std::string &url, const std::string &body) {
    return client.post(url, body);
  };
  REQUIRE_THROWS_AS(acquire("", "{}"), std::runtime_error);
  REQUIRE_THROWS_AS(acquire("http://127.0.0.1:70000/path", "{}"),
                    std::runtime_error);
}

TEST_CASE("HttpClient: connection failure throws", "[http-client]") {
  HttpClient client(2);
  auto acquire = [&client]() {
    return client.post("http://127.0.0.1:1/none", R"({})");
  };
  REQUIRE_THROWS_AS(acquire(), std::runtime_error);
}

TEST_CASE("HttpClient: https client setup and connection failure",
          "[http-client]") {
  HttpClient client(2, true, true, true);
  auto acquire = [&client]() {
    return client.get("https://127.0.0.1:1/secure");
  };
  REQUIRE_THROWS_AS(acquire(), std::runtime_error);
}

TEST_CASE("HttpClient: invalidate marks the client unusable", "[http-client]") {
  HttpClient client(5);
  REQUIRE(client.is_valid());
  client.invalidate();
  REQUIRE_FALSE(client.is_valid());
}

TEST_CASE("HttpClient: static instance counters track lifecycle",
          "[http-client]") {
  const int instances_before = HttpClient::g_instance_count.load();
  const uint64_t created_before = HttpClient::g_total_created.load();
  const uint64_t destroyed_before = HttpClient::g_total_destroyed.load();
  {
    HttpClient local(5);
    REQUIRE(HttpClient::g_instance_count.load() == instances_before + 1);
    REQUIRE(HttpClient::g_total_created.load() == created_before + 1);
  }
  REQUIRE(HttpClient::g_instance_count.load() == instances_before);
  REQUIRE(HttpClient::g_total_destroyed.load() == destroyed_before + 1);
}

TEST_CASE("HttpClient: make_error_json and make_error_response helpers",
          "[http-client]") {
  const auto json = make_error_json("boom");
  REQUIRE(json.is_object());
  REQUIRE(json["error"] == "boom");
  const auto response = make_error_response(502, "bad gateway");
  REQUIRE(response.m_status == 502);
  REQUIRE(response.m_body == R"({"error":"bad gateway"})");
}

// ---------------------------------------------------------------------------
// HttpClientPool
// ---------------------------------------------------------------------------

TEST_CASE("HttpClientPool: acquire creates a connection", "[http-client-pool]") {
  HttpClientPool pool(2, 1, 1);
  auto client = pool.acquire_connection();
  REQUIRE(client != nullptr);
  REQUIRE(pool.total_clients() == 1);
  REQUIRE(pool.active_clients() == 1);
  REQUIRE(pool.available_count() == 0);
  pool.release_connection(std::move(client));
}

TEST_CASE("HttpClientPool: release returns connection and it is reused",
          "[http-client-pool]") {
  HttpClientPool pool(2, 1, 1);
  auto client = pool.acquire_connection();
  pool.release_connection(std::move(client));
  REQUIRE(pool.active_clients() == 0);
  REQUIRE(pool.available_count() == 1);
  REQUIRE(pool.total_clients() == 1);

  auto reused = pool.acquire_connection();
  REQUIRE(pool.total_clients() == 1);
  REQUIRE(pool.active_clients() == 1);
  REQUIRE(pool.available_count() == 0);
  pool.release_connection(std::move(reused));
}

TEST_CASE("HttpClientPool: respects max pool size", "[http-client-pool]") {
  HttpClientPool pool(2, 1, 1);
  auto a = pool.acquire_connection();
  auto b = pool.acquire_connection();
  REQUIRE(pool.total_clients() == 2);
  REQUIRE(pool.active_clients() == 2);

  pool.release_connection(std::move(a));
  pool.release_connection(std::move(b));
  REQUIRE(pool.active_clients() == 0);
  REQUIRE(pool.available_count() == 2);
  REQUIRE(pool.total_clients() == 2);
}

TEST_CASE("HttpClientPool: acquire timeout throws and counts metric",
          "[http-client-pool]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &timeouts = prometheus::BuildCounter()
                       .Name("test_pool_timeouts")
                       .Help("http client pool acquisition timeouts")
                       .Register(*registry)
                       .Add({});
  PoolMetrics metrics;
  metrics.m_acquisition_timeouts = std::ref(timeouts);

  HttpClientPool pool(1, 1, 1);
  pool.set_metrics(metrics);

  auto only = pool.acquire_connection();
  REQUIRE(only != nullptr);

  auto acquire = [&pool]() { return pool.acquire_connection(); };
  REQUIRE_THROWS_AS(acquire(), std::runtime_error);
  REQUIRE(timeouts.Value() == 1);

  pool.release_connection(std::move(only));
  auto again = pool.acquire_connection();
  REQUIRE(again != nullptr);
  REQUIRE(pool.total_clients() == 1);
  pool.release_connection(std::move(again));
}

TEST_CASE("HttpClientPool: waiter blocked on full pool is woken by release",
          "[http-client-pool]") {
  HttpClientPool pool(1, 1, 3);
  auto held = pool.acquire_connection();
  REQUIRE(pool.total_clients() == 1);

  auto waiter =
      std::async(std::launch::async,
                 [&pool]() { return pool.acquire_connection(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  pool.release_connection(std::move(held));
  auto acquired = waiter.get();
  REQUIRE(acquired != nullptr);
  REQUIRE(pool.total_clients() == 1);
  REQUIRE(pool.active_clients() == 1);
  REQUIRE(pool.available_count() == 0);
  pool.release_connection(std::move(acquired));
}

TEST_CASE("HttpClientPool: release invalid connection destroys it",
          "[http-client-pool]") {
  HttpClientPool pool(2, 1, 1);
  auto client = pool.acquire_connection();
  client->invalidate();
  pool.release_connection(std::move(client));
  REQUIRE(pool.active_clients() == 0);
  REQUIRE(pool.total_clients() == 0);
  REQUIRE(pool.available_count() == 0);
}

TEST_CASE("HttpClientPool: stale idle connection is evicted",
          "[http-client-pool]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &stale = prometheus::BuildCounter()
                    .Name("test_pool_stale")
                    .Help("http client pool stale evictions")
                    .Register(*registry)
                    .Add({});
  PoolMetrics metrics;
  metrics.m_stale_evictions = std::ref(stale);

  HttpClientPool pool(2, 1, 1, true, false, false, "", 1);
  pool.set_metrics(metrics);

  auto client = pool.acquire_connection();
  pool.release_connection(std::move(client));
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));

  auto acquired = pool.acquire_connection();
  REQUIRE(acquired != nullptr);
  REQUIRE(stale.Value() == 1);
  REQUIRE(pool.total_clients() == 1);
  REQUIRE(pool.available_count() == 0);
  pool.release_connection(std::move(acquired));
}

TEST_CASE("HttpClientPool: metrics reflect active, available and counters",
          "[http-client-pool]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &active = prometheus::BuildGauge()
                     .Name("test_pool_active")
                     .Help("active pool clients")
                     .Register(*registry)
                     .Add({});
  auto &available = prometheus::BuildGauge()
                        .Name("test_pool_available")
                        .Help("available pool clients")
                        .Register(*registry)
                        .Add({});
  auto &acquisitions = prometheus::BuildCounter()
                           .Name("test_pool_acquisitions")
                           .Help("acquisitions")
                           .Register(*registry)
                           .Add({});
  auto &releases = prometheus::BuildCounter()
                       .Name("test_pool_releases")
                       .Help("releases")
                       .Register(*registry)
                       .Add({});
  auto &duration = prometheus::BuildHistogram()
                       .Name("test_pool_duration")
                       .Help("acquisition duration")
                       .Register(*registry)
                       .Add({}, prometheus::Histogram::BucketBoundaries{
                                    0.001, 0.01, 0.1});
  PoolMetrics metrics;
  metrics.m_active_clients = std::ref(active);
  metrics.m_available_clients = std::ref(available);
  metrics.m_acquisitions = std::ref(acquisitions);
  metrics.m_releases = std::ref(releases);
  metrics.m_acquisition_duration = std::ref(duration);

  HttpClientPool pool(2, 1, 1);
  pool.set_metrics(metrics);

  auto client = pool.acquire_connection();
  REQUIRE(active.Value() == 1);
  REQUIRE(available.Value() == 0);
  REQUIRE(acquisitions.Value() == 1);
  REQUIRE(duration.Collect().histogram.sample_count >= 1);

  pool.release_connection(std::move(client));
  REQUIRE(active.Value() == 0);
  REQUIRE(available.Value() == 1);
  REQUIRE(releases.Value() == 1);

  auto reused = pool.acquire_connection();
  REQUIRE(acquisitions.Value() == 2);
  REQUIRE(active.Value() == 1);
  REQUIRE(pool.total_clients() == 1);
  pool.release_connection(std::move(reused));
}

TEST_CASE("HttpClientPool: release of null pointer is a no-op",
          "[http-client-pool]") {
  HttpClientPool pool(2, 1, 1);
  std::unique_ptr<HttpClient> empty;
  REQUIRE_NOTHROW(pool.release_connection(std::move(empty)));
  REQUIRE(pool.total_clients() == 0);
  REQUIRE(pool.active_clients() == 0);
}

TEST_CASE("HttpClientPool: pool full on release destroys connection",
          "[http-client-pool]") {
  HttpClientPool pool(1, 1, 1);

  auto c1 = pool.acquire_connection();
  REQUIRE(pool.total_clients() == 1);
  auto c4 = std::make_unique<HttpClient>(1, true, false, false, "");
  pool.release_connection(std::move(c1));
  REQUIRE(pool.total_clients() == 1);
  REQUIRE(pool.available_count() == 1);
  pool.release_connection(std::move(c4));
  REQUIRE(pool.total_clients() == 0);
  REQUIRE(pool.available_count() == 1);
}
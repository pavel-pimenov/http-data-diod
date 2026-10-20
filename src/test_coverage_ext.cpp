// Unit tests closing coverage gaps in the remaining header-only and small
// .cpp modules: exceptions, ScopedMetrics, ScopedProfiler, pool_executor,
// metrics_manager, tracing_helpers, stats_page, trace_context_extractor and
// ScopedRequestContext. These are dependency-light (no NATS / DB) and are
// compiled into the test_components target alongside the existing suites.

#include "common_utils.hpp"
#include "config.hpp"
#include "crash_handler.hpp"
#include "db_query_executor.hpp"
#include "db_query_executor_base.hpp"
#include "dynamic_labeled_family.hpp"
#include "error_types.hpp"
#include "exceptions.hpp"
#include "httplib/httplib.h"
#include "json_utils.hpp"
#include "metrics_history.hpp"
#include "logger.hpp"
#include "metrics_manager.hpp"
#include "tracing_metrics.hpp"
#include "nlohmann/json.hpp"
#include "pool_executor.hpp"
#include "prometheus/counter.h"
#include "prometheus/family.h"
#include "prometheus/gauge.h"
#include "prometheus/histogram.h"
#include "prometheus/metric_type.h"
#include "prometheus/registry.h"
#include "prometheus/summary.h"
#include "rate_limiter_per_ip.hpp"
#include "scoped_metrics.hpp"
#include "scoped_profiler.hpp"
#include "stats_page.hpp"
#include "string_utils.hpp"
#include "test_metrics_support.hpp"
#include "test_wait_support.hpp"
#include "thread_pool_wrapper.hpp"
#include "time_utils.hpp"
#include "timed_task_queue.hpp"
#include "trace_context_extractor.hpp"
#include "tracing_helpers.hpp"
#include "url_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <ctime>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ============================================================================
// exceptions.hpp
// ============================================================================

TEST_CASE("StringUtils: to_lower lowercases ASCII and passes non-ASCII through",
          "[string-utils]") {
  REQUIRE(to_lower("Hello WORLD") == "hello world");
  REQUIRE(to_lower("AbC-123") == "abc-123");
  REQUIRE(to_lower("") == "");
}

namespace {
class ReadyExec final : public DbQueryExecutor {
public:
  bool init() override { return true; }
  [[nodiscard]] bool is_ready() const override {
    return DbQueryExecutor::is_ready();
  }
  [[nodiscard]] int default_timeout_ms() const override { return 1000; }
  [[nodiscard]] int default_max_rows() const override { return 10; }
  [[nodiscard]] const std::string &db_name() const override {
    static const std::string g_mock_db_name{"mock"};
    return g_mock_db_name;
  }
  json execute_query(const std::string &, const json &, int, int,
                     int &) override {
    return json::object();
  }
  bool ping(int) override { return true; }
  void set_error_metrics(prometheus::Family<prometheus::Counter> *) override {}
  void set_pool_metrics(prometheus::Family<prometheus::Gauge> *) override {}
  void set_pool_saturation_metrics(prometheus::Family<prometheus::Gauge> *,
                                  prometheus::Family<prometheus::Counter> *,
                                  prometheus::Family<prometheus::Histogram> *)
      override {}
};
} // namespace

TEST_CASE("DbQueryExecutor: default is_ready returns true", "[db-exec]") {
  ReadyExec exec;
  REQUIRE(exec.is_ready());
  REQUIRE(exec.init());
  REQUIRE(exec.ping(1));
  REQUIRE(exec.default_timeout_ms() == 1000);
  REQUIRE(exec.default_max_rows() == 10);
  exec.set_pool_saturation_metrics(nullptr, nullptr, nullptr);
  REQUIRE(exec.db_name() == "mock");
}

TEST_CASE("Exceptions: L2ProxyException derives from runtime_error",
          "[exceptions]") {
  L2ProxyException const ex("boom");
  REQUIRE_THROWS_AS(throw ex, std::runtime_error);
  REQUIRE(std::string(ex.what()) == "boom");
}

// ============================================================================
// scoped_metrics.hpp
// ============================================================================

TEST_CASE("ScopedMetrics: increments the counter on scope exit",
          "[scoped-metrics]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &counter = test_support::make_counter(
      *registry, "scoped_count", "scoped test");
  {
    ScopedMetrics const guard(counter);
    REQUIRE(counter.Value() == 0.0);
  }
  REQUIRE(counter.Value() == 1.0);
}

// ============================================================================
// scoped_profiler.hpp
// ============================================================================

TEST_CASE("ScopedProfiler: observes the duration histogram on scope exit",
          "[scoped-profiler]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &hist = test_support::make_histogram(
      *registry, "scoped_prof", "scoped test", std::vector<double>{0.001, 1.0});
  {
    ScopedProfiler const profiler(hist);
    REQUIRE(hist.Collect().histogram.sample_count == 0);
  }
  const auto collected = hist.Collect();
  REQUIRE(collected.histogram.sample_count == 1);
  REQUIRE(collected.histogram.sample_sum > 0.0);
}

TEST_CASE("ScopedLabeledProfiler: null collector is a no-op",
          "[scoped-profiler]") {
  ScopedLabeledProfiler const profiler(nullptr, "client-1");
  REQUIRE_NOTHROW(ScopedLabeledProfiler(nullptr, "client-2"));
}

TEST_CASE("ScopedLabeledProfiler: records under the label value",
          "[scoped-profiler]") {
  DynamicLabeledFamily<prometheus::Histogram> family(
      "client_id",
      std::vector<DynamicLabeledFamily<prometheus::Histogram>::Series>{
          {"client_latency", "per client latency"}},
      {}, 0, 10, std::vector<double>{0.001, 1.0});
  {
    ScopedLabeledProfiler const profiler(&family, "alice");
  }
  auto families = family.Collect();
  REQUIRE(families.size() == 1);
  REQUIRE(families[0].metric.size() == 1);
  REQUIRE(families[0].metric[0].label[0].value == "alice");
  REQUIRE(families[0].metric[0].histogram.sample_count == 1);
}

// ============================================================================
// pool_executor.hpp
// ============================================================================

namespace {

// Minimal mock pool satisfying the requirements of
// execute_http_command_with_status: client_type, acquire_connection(),
// release_connection(), and the client's is_valid/get_last_status_code/
// invalidate.
struct MockHttpClient {
  bool m_valid = true;
  int m_last_status = 200;
  bool m_invalidated = false;

  [[nodiscard]] bool is_valid() const { return m_valid; }
  [[nodiscard]] int get_last_status_code() const { return m_last_status; }
  void invalidate() { m_invalidated = true; }
};

struct MockPool {
  using client_type = MockHttpClient;
  int m_acquires = 0;
  int m_releases = 0;

  std::unique_ptr<MockHttpClient> acquire_connection() {
    ++m_acquires;
    return std::make_unique<MockHttpClient>();
  }
  void release_connection(std::unique_ptr<MockHttpClient>) { ++m_releases; }
};

std::string run_probe(MockHttpClient *) { return "probe"; }

} // namespace

TEST_CASE("Pool executor: null pool throws", "[pool-executor]") {
  using FuncT = decltype(run_probe);
  REQUIRE_THROWS_AS(
      (execute_http_command_with_status<MockPool, FuncT>(nullptr, run_probe)),
      std::runtime_error);
}

TEST_CASE("Pool executor: pool returning no connection throws",
          "[pool-executor]") {
  // A pool that hands out nothing (all connections invalid/closed) must be a
  // hard error rather than a null dereference in the caller.
  struct NullPool {
    using client_type = MockHttpClient;
    static std::unique_ptr<MockHttpClient> acquire_connection() {
      return nullptr;
    }
    void release_connection(std::unique_ptr<MockHttpClient>) {}
  };
  NullPool pool;
  REQUIRE_THROWS_AS(
      execute_http_command_with_status(
          &pool, [](MockHttpClient *) { return std::string("never reached"); }),
      std::runtime_error);
}

TEST_CASE("Pool executor: returns result and status code", "[pool-executor]") {
  MockPool pool;
  auto result = execute_http_command_with_status(&pool, [](MockHttpClient *c) {
    c->m_last_status = 201;
    return std::string("response body");
  });
  REQUIRE(result.first == "response body");
  REQUIRE(result.second == 201);
  REQUIRE(pool.m_acquires == 1);
  REQUIRE(pool.m_releases == 1);
}

TEST_CASE("Pool executor: invalid client is released and throws",
          "[pool-executor]") {
  struct InvalidPool {
    using client_type = MockHttpClient;
    int m_releases = 0;
    static std::unique_ptr<MockHttpClient> acquire_connection() {
      auto c = std::make_unique<MockHttpClient>();
      c->m_valid = false;
      return c;
    }
    void release_connection(std::unique_ptr<MockHttpClient>) { ++m_releases; }
  };
  InvalidPool pool;
  REQUIRE_THROWS_AS(
      execute_http_command_with_status(
          &pool, [](MockHttpClient *) { return std::string("never reached"); }),
      std::runtime_error);
  REQUIRE(pool.m_releases == 1);
}

TEST_CASE("Pool executor: exception in func invalidates and releases then "
          "rethrows",
          "[pool-executor]") {
  MockPool pool;
  REQUIRE_THROWS_AS(execute_http_command_with_status(
                        &pool,
                        [](MockHttpClient *) -> int {
                          throw std::runtime_error("func failed");
                        }),
                    std::runtime_error);
  REQUIRE(pool.m_acquires == 1);
  REQUIRE(pool.m_releases == 1);
}

// ============================================================================
// metrics_manager.cpp
// ============================================================================

TEST_CASE("MetricsManager: creates and increments a counter",
          "[metrics-manager]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &counter = MetricsManager::create_counter(registry, "mm_count", "mm");
  counter.Increment();
  REQUIRE(counter.Value() == 1.0);
}

TEST_CASE("MetricsManager: creates a gauge and sets a value",
          "[metrics-manager]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &gauge = MetricsManager::create_gauge(registry, "mm_gauge", "mm");
  gauge.Set(42.0);
  REQUIRE(gauge.Value() == 42.0);
}

TEST_CASE("MetricsManager: creates a histogram and observes a value",
          "[metrics-manager]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &hist = MetricsManager::create_histogram(registry, "mm_hist", "mm",
                                                std::vector<double>{0.5, 1.0});
  hist.Observe(0.75);
  const auto collected = hist.Collect();
  REQUIRE(collected.histogram.sample_count == 1);
  REQUIRE(collected.histogram.sample_sum == 0.75);
}

TEST_CASE("MetricsManager: family creators behave as collectables",
          "[metrics-manager]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &cfam = MetricsManager::create_counter_family(registry, "mm_cfam", "");
  REQUIRE(cfam.Collect().empty());
  cfam.Add({}).Increment();
  auto ccollected = cfam.Collect();
  REQUIRE(ccollected.front().metric.size() == 1);
  REQUIRE(ccollected.front().metric[0].counter.value == 1.0);

  auto &gfam = MetricsManager::create_gauge_family(registry, "mm_gfam", "");
  REQUIRE(gfam.Collect().empty());

  auto &hfam = MetricsManager::create_histogram_family(
      registry, "mm_hfam", "");
  hfam.Add({{"db", "x"}}, std::vector<double>{0.1, 1.0}).Observe(0.2);
  auto hcollected = hfam.Collect();
  REQUIRE(hcollected.front().metric.size() == 1);
  REQUIRE(hcollected.front().metric[0].histogram.sample_count == 1);
}

TEST_CASE("MetricsManager: array-arg creators forward bucket bounds",
          "[metrics-manager]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &hist = MetricsManager::create_histogram(
      registry, "mm_hist_arr", "", histogram_buckets::g_buckets.m_latency_ms_to_10s);
  hist.Observe(5.0);
  auto &fam = MetricsManager::create_histogram_family(
      registry, "mm_hfam_arr", "");
  REQUIRE(fam.Collect().empty());
}

TEST_CASE("MetricsManager: record_db_request_metrics increments labelled "
          "series",
          "[metrics-manager]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &fam = MetricsManager::create_counter_family(registry, "db_total", "");
  record_db_request_metrics(fam, "oracle", "query", 200);
  record_db_request_metrics(fam, "oracle", "query", 200);
  record_db_request_metrics(fam, "oracle", "", 500);
  auto collected = fam.Collect();
  REQUIRE(collected.front().metric.size() == 2);
  double seen_ok = 0.0;
  double seen_unknown = 0.0;
  for (const auto &m : collected.front().metric) {
    if (m.counter.value == 2.0) {
      seen_ok = m.counter.value;
    }
    if (m.counter.value == 1.0) {
      seen_unknown = m.counter.value;
    }
    for (const auto &label : m.label) {
      if (label.name == "type") {
        if (label.value == "query") {
          seen_ok = m.counter.value;
        }
        if (label.value == "unknown") {
          seen_unknown = m.counter.value;
        }
      }
    }
  }
  // The two identical calls land on one series, the empty-type call maps to
  // "unknown".
  REQUIRE(seen_ok == 2.0);
  REQUIRE(seen_unknown == 1.0);
}

TEST_CASE("MetricsManager: observe_db_request_duration observes seconds",
          "[metrics-manager]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &fam = MetricsManager::create_histogram_family(
      registry, "db_dur", "");
  observe_db_request_duration(fam, "pg", 1'000'000, 1'100'000);
  auto collected = fam.Collect();
  REQUIRE(collected.front().metric.size() == 1);
  REQUIRE(collected.front().metric[0].histogram.sample_count == 1);
  REQUIRE_THAT(collected.front().metric[0].histogram.sample_sum,
               Catch::Matchers::WithinRel(0.1, 0.001));
}

// ============================================================================
// tracing_helpers.hpp (pure helpers)
// ============================================================================

TEST_CASE("Tracing helpers: proxy_service_name prefixes the mode",
          "[tracing-helpers]") {
  REQUIRE(proxy_service_name("proxy") == "l2-proxy-proxy");
  REQUIRE(proxy_service_name("worker") == "l2-proxy-worker");
}

TEST_CASE("Tracing helpers: get_traceparent_header reads the header",
          "[tracing-helpers]") {
  httplib::Headers headers;
  REQUIRE(get_traceparent_header(headers) == "");
  headers.emplace("traceparent", "00-abc-def-01");
  REQUIRE(get_traceparent_header(headers) == "00-abc-def-01");
}

TEST_CASE("Tracing helpers: resolve_trace_id uses context then generator",
          "[tracing-helpers]") {
  TraceContext ctx;
  ctx.m_trace_id = "known-trace";
  REQUIRE(resolve_trace_id(nullptr, ctx) == "known-trace");
  TraceContext const empty;
  REQUIRE(resolve_trace_id(nullptr, empty) == "");
}

TEST_CASE("Tracing helpers: make_span_and_traceparent with null tracer",
          "[tracing-helpers]") {
  TraceContext ctx;
  ctx.m_trace_id = "";
  ctx.m_traceparent_header = "";
  auto [span_id, tp] = make_span_and_traceparent(nullptr, ctx);
  REQUIRE(span_id == "");
  REQUIRE(tp == "");

  TraceContext with_tp;
  with_tp.m_trace_id = "";
  with_tp.m_traceparent_header = "00-abc-def-01";
  auto [sp2, tp2] = make_span_and_traceparent(nullptr, with_tp, "hint");
  REQUIRE(sp2 == "hint");
  REQUIRE(tp2 == "00-abc-def-01");
}

TEST_CASE(
    "Tracing helpers: set_traceparent_response_header sets when non-empty",
    "[tracing-helpers]") {
  httplib::Response res;
  TraceContext const empty;
  set_traceparent_response_header(res, empty);
  REQUIRE(res.get_header_value("traceparent") == "");

  TraceContext ctx;
  ctx.m_traceparent_header = "00-a-b-01";
  set_traceparent_response_header(res, ctx);
  REQUIRE(res.get_header_value("traceparent") == "00-a-b-01");
}

TEST_CASE("Tracing helpers: begin_request_trace with null tracer",
          "[tracing-helpers]") {
  httplib::Headers const headers;
  std::string inlet;
  const TraceContext ctx =
      begin_request_trace(nullptr, headers, "req-1", "/p", 1234, inlet);
  REQUIRE(ctx.m_trace_id == "");
  REQUIRE(ctx.m_span_id == "");
  REQUIRE(inlet == "");
}

TEST_CASE(
    "Tracing helpers: TraceContextHelper extract_from_raw with null tracer",
    "[tracing-helpers]") {
  const TraceContext ctx =
      TraceContextHelper::extract_from_raw("", nullptr, "test");
  REQUIRE(ctx.m_trace_id == "");
}

TEST_CASE("Tracing helpers: make_span_and_traceparent with traced context and "
          "hint",
          "[tracing-helpers]") {
  TraceContext ctx;
  ctx.m_trace_id = "abc123";
  ctx.m_traceparent_header = "00-abc123-def-01";
  auto [span_id, tp] = make_span_and_traceparent(nullptr, ctx, "my-span");
  REQUIRE(span_id == "my-span");
  REQUIRE(tp == "00-abc123-def-01");
}

// ============================================================================
// stats_page.hpp (format helpers + build_stats_html)
// ============================================================================

TEST_CASE("Stats page: format_metric_value per type", "[stats-page-ext]") {
  prometheus::ClientMetric counter;
  counter.counter.value = 12.5;
  REQUIRE(format_metric_value(counter, prometheus::MetricType::Counter) ==
          "12.5");

  prometheus::ClientMetric gauge;
  gauge.gauge.value = 3.0;
  REQUIRE(format_metric_value(gauge, prometheus::MetricType::Gauge) == "3");

  prometheus::ClientMetric untyped;
  untyped.untyped.value = 3.0;
  REQUIRE(format_metric_value(untyped, prometheus::MetricType::Untyped) == "3");

  prometheus::ClientMetric hist;
  hist.histogram.sample_count = 5;
  hist.histogram.sample_sum = 2.5;
  const std::string h =
      format_metric_value(hist, prometheus::MetricType::Histogram);
  REQUIRE(h.find("count=5") != std::string::npos);
  REQUIRE(h.find("sum=2.5") != std::string::npos);

  prometheus::ClientMetric summary;
  summary.summary.sample_count = 7;
  summary.summary.sample_sum = 1.0;
  const std::string s =
      format_metric_value(summary, prometheus::MetricType::Summary);
  REQUIRE(s.find("count=7") != std::string::npos);
  REQUIRE(s.find("sum=1") != std::string::npos);
}

TEST_CASE("Stats page: format_labels renders braces", "[stats-page-ext]") {
  REQUIRE(format_labels({}) == "");
  const std::vector<prometheus::ClientMetric::Label> labels = {{"job", "proxy"},
                                                               {"state", "ok"}};
  REQUIRE(format_labels(labels) == "{job=proxy, state=ok}");
}

TEST_CASE("Stats page: build_stats_html renders banner and tiles",
          "[stats-page-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &gauge = MetricsManager::create_gauge(registry, "health_ready", "ready");
  gauge.Set(1.0);
  auto &counter = test_support::make_counter(
      *registry, "requests_total", "requests");
  counter.Increment();

  const std::string html =
      build_stats_html("proxy-test", registry, nullptr, 30);
  REQUIRE(html.find("OPERATIONAL") != std::string::npos);
  REQUIRE(html.find("proxy-test") != std::string::npos);
  REQUIRE(html.find("health_ready") != std::string::npos);
  REQUIRE(html.find("requests_total") != std::string::npos);
}

TEST_CASE("Stats page: dense labeled family renders the +N more overflow row",
          "[stats-page-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &family = test_support::make_gauge_family(
      *registry, "dense_series", "dense family");
  // kMaxSeriesPerTile is 6 in build_stats_html: 8 series must collapse to the
  // first 6 plus a "+2 more" overflow row.
  for (int i = 0; i < 8; ++i) {
    family.Add({{"shard", std::to_string(i)}}).Set(static_cast<double>(i));
  }

  const std::string html =
      build_stats_html("proxy-test", registry, nullptr, 30);
  REQUIRE(html.find("dense_series") != std::string::npos);
  REQUIRE(html.find("+2 more") != std::string::npos);
}

TEST_CASE("Stats page: build_stats_html flags degraded when health not ready",
          "[stats-page-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &gauge = MetricsManager::create_gauge(registry, "health_ready", "ready");
  gauge.Set(0.0);
  const std::string html =
      build_stats_html("proxy-test", registry, nullptr, 30);
  REQUIRE(html.find("DEGRADED") != std::string::npos);
}

TEST_CASE("Stats page: escape_html escapes special characters",
          "[stats-page-ext]") {
  REQUIRE(escape_html("a&b") == "a&amp;b");
  REQUIRE(escape_html("<tag>") == "&lt;tag&gt;");
  REQUIRE(escape_html("say \"hi\"") == "say &quot;hi&quot;");
  REQUIRE(escape_html("plain") == "plain");
}

TEST_CASE(
    "Stats page: parse_stats_window handles absent/invalid/clamped values",
    "[stats-page-ext]") {
  std::map<std::string, std::string> const no_window;
  REQUIRE(parse_stats_window(no_window) == 30);
  REQUIRE(parse_stats_window(no_window, 60) == 60);

  std::map<std::string, std::string> const exact{{"window", "15"}};
  REQUIRE(parse_stats_window(exact) == 15);

  std::map<std::string, std::string> const invalid{{"window", "abc"}};
  REQUIRE(parse_stats_window(invalid) == 30);

  std::map<std::string, std::string> const clamped_high{{"window", "999"}};
  REQUIRE(parse_stats_window(clamped_high) == 120);

  std::map<std::string, std::string> const clamped_low{{"window", "0"}};
  REQUIRE(parse_stats_window(clamped_low) == 1);

  std::map<std::string, std::string> const partial{{"window", "7x"}};
  REQUIRE(parse_stats_window(partial) == 7);
}

TEST_CASE("Stats page: build_sparkline_svg returns empty for <2 points",
          "[stats-page-ext]") {
  REQUIRE(build_sparkline_svg({}, false, 30) == "");
  const std::time_t now = std::time(nullptr);
  REQUIRE(build_sparkline_svg({{now, 1.0}}, true, 30) == "");
}

TEST_CASE("Stats page: build_sparkline_svg renders rate and clamps counter "
          "resets",
          "[stats-page-ext]") {
  const std::time_t now = std::time(nullptr);
  const std::vector<std::pair<std::time_t, double>> pts = {
      {now - 2, 10.0}, {now - 1, 5.0}, {now, 8.0}};
  const std::string svg = build_sparkline_svg(pts, true, 30);
  REQUIRE(svg.find("<svg class=\"spark\"") != std::string::npos);
  REQUIRE(svg.find("0,") != std::string::npos);
}

TEST_CASE(
    "Stats page: build_sparkline_svg renders raw gauge and applies window",
    "[stats-page-ext]") {
  const std::time_t now = std::time(nullptr);
  const std::vector<std::pair<std::time_t, double>> pts = {
      {now - 3600, 1.0}, {now - 90, 2.0}, {now - 60, 3.0}};
  const std::string svg = build_sparkline_svg(pts, false, 1);
  REQUIRE(svg.find("<svg class=\"spark\"") != std::string::npos);
}

TEST_CASE("Stats page: build_stats_html flags degraded when nats disconnected",
          "[stats-page-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &nats = MetricsManager::create_gauge(registry, "nats_connected", "nats");
  nats.Set(0.0);
  const std::string html =
      build_stats_html("proxy-test", registry, nullptr, 30);
  REQUIRE(html.find("DEGRADED") != std::string::npos);
}

TEST_CASE("Stats page: build_stats_html caps dense families with more marker",
          "[stats-page-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &family = test_support::make_gauge_family(
      *registry, "per_ip_requests", "per ip");
  for (int i = 0; i < 8; ++i) {
    family.Add({{"ip", "10.0.0." + std::to_string(i)}})
        .Set(static_cast<double>(i));
  }
  const std::string html =
      build_stats_html("proxy-test", registry, nullptr, 30);
  REQUIRE(html.find("+2 more") != std::string::npos);
}

TEST_CASE("Stats page: build_stats_html renders sparklines from history",
          "[stats-page-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &counter = test_support::make_counter(
      *registry, "spark_requests_total", "requests");
  counter.Increment();
  MetricsHistory history(registry, std::chrono::seconds(1), 8, 240);
  history.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(2300));
  history.stop();
  const std::string html =
      build_stats_html("proxy-test", registry, &history, 5);
  REQUIRE(html.find("<div class=\"sparkwrap\"") != std::string::npos);
}

TEST_CASE(
    "Stats page: build_sparkline_svg single sample rate and zero delta",
    "[stats-page-ext]") {
  // Two points as_rate produce exactly one rate sample (vals.size()==1 -> x=0)
  // and the dt<=0 guard is exercised with identical timestamps.
  const std::time_t now = std::time(nullptr);
  const std::vector<std::pair<std::time_t, double>> same_ts = {{now, 5.0},
                                                               {now, 7.0}};
  const std::string svg = build_sparkline_svg(same_ts, true, 30);
  REQUIRE(svg.find("<svg class=\"spark\"") != std::string::npos);
  REQUIRE(svg.find("0,25") != std::string::npos);
}

TEST_CASE("Stats page: build_sparkline_svg flat range uses unit range",
          "[stats-page-ext]") {
  const std::time_t now = std::time(nullptr);
  const std::vector<std::pair<std::time_t, double>> flat = {
      {now - 2, 4.0}, {now - 1, 4.0}, {now, 4.0}};
  const std::string svg = build_sparkline_svg(flat, false, 30);
  REQUIRE(svg.find("<svg class=\"spark\"") != std::string::npos);
  REQUIRE(svg.find("94,25") != std::string::npos);
}

TEST_CASE("Stats page: build_stats_html omits tile help when empty",
          "[stats-page-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &plain = test_support::make_gauge_family(*registry, "plain_gauge", "");
  plain.Add({}).Set(1.0);
  const std::string html =
      build_stats_html("proxy-test", registry, nullptr, 30);
  REQUIRE(html.find("plain_gauge") != std::string::npos);
  REQUIRE(html.find("class=\"thelp\"") == std::string::npos);
}

TEST_CASE(
    "Stats page: build_stats_html renders labeled gauge sparklines raw",
    "[stats-page-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &family = test_support::make_gauge_family(
      *registry, "spark_per_ip", "per ip gauge");
  family.Add({{"ip", "0.0.0.1"}}).Set(100.0);
  family.Add({{"ip", "9.9.9.9"}}).Set(1.0);
  MetricsHistory history(registry, std::chrono::seconds(1), 8, 240);
  history.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(2300));
  history.stop();
  const std::string html =
      build_stats_html("proxy-test", registry, &history, 5);
  REQUIRE(html.find("<div class=\"sparkwrap\"") != std::string::npos);
}

TEST_CASE("Stats page: build_stats_html renders extra registry tiles",
          "[stats-page-ext]") {
  auto primary = std::make_shared<prometheus::Registry>();
  auto &p_gauge = MetricsManager::create_gauge(primary, "health_ready", "ready");
  p_gauge.Set(1.0);
  auto &p_cnt = test_support::make_counter(
      *primary, "proxy_requests_total", "proxy requests");
  p_cnt.Increment();

  auto extra = std::make_shared<prometheus::Registry>();
  auto &t_cnt = test_support::make_counter(
      *extra, "l2_tracing_spans_sent_total", "spans sent");
  t_cnt.Increment();
  auto &s_gauge = MetricsManager::create_gauge(
      extra, "l2_worker_sentry_queue_size", "sentry queue");
  s_gauge.Set(0.0);

  MetricsHistory extra_history(extra, std::chrono::seconds(1), 8, 240);
  extra_history.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(2300));
  extra_history.stop();

  const std::string html =
      build_stats_html("l2-worker", primary, nullptr, 30, extra,
                       &extra_history);
  // Primary registry tiles still rendered alongside the extra ones.
  REQUIRE(html.find("proxy_requests_total") != std::string::npos);
  REQUIRE(html.find("health_ready") != std::string::npos);
  // Extra-registry families render their own tiles and sparklines from the
  // extra history (the primary history does not know these families).
  REQUIRE(html.find("l2_tracing_spans_sent_total") != std::string::npos);
  REQUIRE(html.find("l2_worker_sentry_queue_size") != std::string::npos);
  REQUIRE(html.find("<div class=\"sparkwrap\"") != std::string::npos);
}

TEST_CASE("Stats page: build_stats_html extra registry skips empty families",
          "[stats-page-ext]") {
  auto primary = std::make_shared<prometheus::Registry>();
  auto &p_gauge = MetricsManager::create_gauge(primary, "health_ready", "ready");
  p_gauge.Set(1.0);
  auto extra = std::make_shared<prometheus::Registry>();
  // Family registered but never observed: render_tile must bail out early.
  prometheus::BuildGauge().Name("l2_tracing_queue_size").Help("queue").Register(
      *extra);
  auto &live = MetricsManager::create_gauge(extra, "l2_tracing_queue_size_live",
                                            "queue live");
  live.Set(2.0);

  const std::string html =
      build_stats_html("l2-worker", primary, nullptr, 30, extra, nullptr);
  REQUIRE(html.find("l2_tracing_queue_size_live") != std::string::npos);
  // No tile for the metric-less family.
  REQUIRE(html.find(">l2_tracing_queue_size<") == std::string::npos);
  // Rendered without extra history -> no sparkline from the extra registry.
  REQUIRE(html.find("<div class=\"sparkwrap\"") == std::string::npos);
}

TEST_CASE("MetricsHistory: samples registry into a bounded ring buffer",
          "[metrics-history]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &cnt = test_support::make_counter(*registry, "mh_count", "h");
  cnt.Increment();
  auto &gauge = test_support::make_gauge_family(*registry, "mh_gau", "h");
  gauge.Add({{"ip", "1"}}).Set(2.0);
  gauge.Add({{"ip", "2"}}).Set(3.0);
  auto &sum = prometheus::BuildSummary()
                  .Name("mh_sum")
                  .Help("h")
                  .Register(*registry)
                  .Add({}, prometheus::Summary::Quantiles{{0.5, 0.05}});
  sum.Observe(3.0);

  MetricsHistory history(registry, std::chrono::seconds(1), 1, 2);
  history.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(2300));
  history.stop();

  REQUIRE(history.has_family("mh_count"));
  REQUIRE_FALSE(history.has_family("mh_missing"));
  REQUIRE(history.get_series("mh_missing", 5).empty());

  const auto count_series = history.get_series("mh_count", 5);
  REQUIRE(count_series.size() == 1);
  REQUIRE(count_series[0].m_labels == "");
  REQUIRE(count_series[0].m_points.size() >= 1);
  REQUIRE(count_series[0].m_points.size() <= 2);

  const auto gauge_series = history.get_series("mh_gau", 5);
  REQUIRE(gauge_series.size() == 1);
  REQUIRE((gauge_series[0].m_labels == "{ip=1}" ||
           gauge_series[0].m_labels == "{ip=2}"));

  REQUIRE(history.has_family("mh_sum"));
}

TEST_CASE("MetricsHistory: start is idempotent and null registry is tolerated",
          "[metrics-history]") {
  auto registry = std::make_shared<prometheus::Registry>();
  MetricsHistory history(registry, std::chrono::seconds(1), 8, 240);
  history.start();
  history.start();
  history.stop();
  history.stop();
  REQUIRE_FALSE(history.has_family("whatever"));

  MetricsHistory empty(nullptr, std::chrono::seconds(1), 8, 240);
  empty.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  empty.stop();
}

// ============================================================================
// trace_context_extractor.cpp
// ============================================================================

TEST_CASE("Trace context extractor: null tracer returns empty context",
          "[trace-context-extractor]") {
  httplib::Request const req;
  std::string backend_push_span_id;
  std::string traceparent_for_backend;
  const TraceContext ctx = extract_trace_context(
      req, nullptr, backend_push_span_id, traceparent_for_backend);
  REQUIRE(ctx.m_trace_id == "");
  REQUIRE(ctx.m_span_id == "");
}

TEST_CASE("Trace context extractor: forwards traceparent header key",
          "[trace-context-extractor]") {
  httplib::Request req;
  req.headers.emplace("traceparent", "00-aa-bb-01");
  std::string backend_push_span_id;
  std::string traceparent_for_backend;
  const TraceContext ctx = extract_trace_context(
      req, nullptr, backend_push_span_id, traceparent_for_backend);
  REQUIRE(ctx.m_traceparent_header == "");
}

// ============================================================================
// ScopedRequestContext (common_utils.hpp)
// ============================================================================

TEST_CASE("ScopedRequestContext: defaults client ip to unknown",
          "[scoped-request-context]") {
  httplib::Request req;
  req.remote_addr = "";
  ScopedRequestContext const ctx(req);
  REQUIRE(ctx.client_ip() == "unknown");
}

TEST_CASE("ScopedRequestContext: uses x-real-ip when present",
          "[scoped-request-context]") {
  httplib::Request req;
  req.remote_addr = "9.9.9.9";
  req.headers.emplace("x-real-ip", "1.2.3.4");
  ScopedRequestContext const ctx(req);
  REQUIRE(ctx.client_ip() == "1.2.3.4");
}

// ============================================================================
// ThreadPoolWrapper (thread_pool_wrapper.hpp)
// ============================================================================

TEST_CASE("ThreadPoolWrapper: NONE runs task synchronously",
          "[thread-pool-wrapper]") {
  ThreadPoolWrapper pool(ThreadPoolWrapper::Type::NONE, 4);
  REQUIRE(pool.queue_size() == 0);
  auto fut = pool.enqueue([](int a, int b) { return a + b; }, 20, 22);
  REQUIRE(fut.get() == 42);
  REQUIRE(pool.queue_size() == 0);
}

TEST_CASE("ThreadPoolWrapper: NONE propagates exceptions",
          "[thread-pool-wrapper]") {
  ThreadPoolWrapper pool(ThreadPoolWrapper::Type::NONE, 1);
  auto fut = pool.enqueue([]() -> int {
    throw std::runtime_error("sync boom");
    return 1;
  });
  REQUIRE_THROWS_AS(fut.get(), std::runtime_error);
}

TEST_CASE("ThreadPoolWrapper: CUSTOM executes on the pool and reuses it",
          "[thread-pool-wrapper]") {
  ThreadPoolWrapper pool(ThreadPoolWrapper::Type::CUSTOM, 2, 8);
  const size_t tasks = 8;
  auto fut = pool.enqueue([]() { return std::this_thread::get_id(); });
  REQUIRE(fut.get() != std::this_thread::get_id());
  std::vector<std::future<int>> results;
  results.reserve(tasks);
  for (size_t i = 0; i < tasks; ++i) {
    results.push_back(pool.enqueue([i]() { return static_cast<int>(i * i); }));
  }
  for (size_t i = 0; i < tasks; ++i) {
    REQUIRE(results[i].get() == static_cast<int>(i * i));
  }
}

TEST_CASE("ThreadPoolWrapper: CUSTOM queue_size reflects pending work",
          "[thread-pool-wrapper]") {
  ThreadPoolWrapper pool(ThreadPoolWrapper::Type::CUSTOM, 1, 2);
  pool.enqueue(
      []() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
  REQUIRE(pool.queue_size() <= 2);
  pool.enqueue([]() {});
  pool.enqueue([]() {});
  REQUIRE(pool.queue_size() <= 2);
}

TEST_CASE("ThreadPoolWrapper: unknown pool type is rejected on enqueue",
          "[thread-pool-wrapper]") {
  // The enum only carries CUSTOM/NONE; an out-of-range value must be refused
  // instead of dereferencing a pool that was never constructed.
  // The test needs an out-of-range enum value on purpose: the point is that
  // the pool refuses it instead of dereferencing a pool that was never built.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  auto const unknown = static_cast<ThreadPoolWrapper::Type>(42);
  ThreadPoolWrapper pool(unknown, 1);
  REQUIRE(pool.queue_size() == 0);
  REQUIRE_THROWS_AS(pool.enqueue([]() { return 1; }), std::runtime_error);
}

// ============================================================================
// CrashHandler (crash_handler.hpp) — non-signal surface
// ============================================================================

TEST_CASE("CrashHandler: default dump dir constant is exposed",
          "[crash-handler]") {
  REQUIRE(std::string(g_default_crash_dump_dir) == "/crash-dumps");
}

// ============================================================================
// common_utils.cpp — remaining logging / counter helpers
// ============================================================================

TEST_CASE("Common utils ext: log_span_to_jaeger no-op with null tracer",
          "[common-utils-ext]") {
  REQUIRE_NOTHROW(log_span_to_jaeger(
      nullptr, "POST", "http://x/query", 200, 1'000, 2'000, "srv", "req-1",
      "trace-1", "span-1", "parent-1", nlohmann::json::object()));
}

TEST_CASE("Common utils ext: log_request_received and log_response_sent",
          "[common-utils-ext]") {
  REQUIRE_NOTHROW(log_request_received("ctx", 42));
  REQUIRE_NOTHROW(log_response_sent("ctx", "req-x", 200));
  REQUIRE_NOTHROW(log_response_sent("ctx", "", 500));
}

TEST_CASE("Common utils ext: increment_and_log_* bump counters",
          "[common-utils-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &recv = test_support::make_counter(*registry, "cu_recv", "h");
  auto &sent = test_support::make_counter(*registry, "cu_sent", "h");
  REQUIRE(recv.Value() == 0.0);
  REQUIRE(sent.Value() == 0.0);
  increment_and_log_request_received(recv, "ctx", 10);
  increment_and_log_response_sent(sent, "ctx", "req-y", 200);
  REQUIRE(recv.Value() == 1.0);
  REQUIRE(sent.Value() == 1.0);
}

TEST_CASE("Common utils ext: handle_processing_error_with_category maps "
          "JSON errors to the dedicated counter",
          "[common-utils-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &inv = test_support::make_counter(*registry, "cu_total", "h");
  auto &json_err = test_support::make_counter(*registry, "cu_json", "h");
  auto &valid_err = test_support::make_counter(*registry, "cu_valid", "h");
  auto &other_err = test_support::make_counter(*registry, "cu_other", "h");
  ProcessingErrorMetrics metrics;
  metrics.m_total_errors = &inv;
  metrics.m_json_errors = &json_err;
  metrics.m_validation_errors = &valid_err;
  metrics.m_other_errors = &other_err;

  handle_processing_error_with_category("invalid json: syntax error", metrics);
  REQUIRE(inv.Value() == 1.0);
  REQUIRE(json_err.Value() == 1.0);
  REQUIRE(valid_err.Value() == 0.0);
  REQUIRE(other_err.Value() == 0.0);

  // No dedicated counter for the decompression category: it falls back to
  // the "other" counter (the category itself still shows up in the log).
  handle_processing_error_with_category("bzip2 decompression failed", metrics);
  REQUIRE(inv.Value() == 2.0);
  REQUIRE(other_err.Value() == 1.0);

  handle_processing_error_with_category(
      "validation failed: unknown field missing", metrics);
  REQUIRE(inv.Value() == 3.0);
  REQUIRE(valid_err.Value() == 1.0);
  REQUIRE(other_err.Value() == 1.0);

  handle_processing_error_with_category("random mystery error", metrics);
  REQUIRE(inv.Value() == 4.0);
  REQUIRE(other_err.Value() == 2.0);
}

TEST_CASE("Common utils ext: handle_processing_error_with_category tolerates "
          "null counters",
          "[common-utils-ext]") {
  ProcessingErrorMetrics const metrics;
  REQUIRE_NOTHROW(
      handle_processing_error_with_category("invalid json: boom", metrics));
}

// ============================================================================
// tracing_helpers.hpp — null-tracer guard branches
// ============================================================================

TEST_CASE("Tracing helpers: JaegerSpanLogger methods no-op with null tracer",
          "[tracing-helpers]") {
  TraceContext ctx;
  ctx.m_trace_id = "abc";
  ctx.m_parent_id = "def";
  ctx.m_span_id = "ghi";
  REQUIRE_NOTHROW(JaegerSpanLogger::log_l2_call(
      nullptr, "POST", "/query", 200, 1, 2, ctx, "proxy", "s1", "p1", "req-1",
      nlohmann::json::object()));
  REQUIRE_NOTHROW(JaegerSpanLogger::log_worker_processing(
      nullptr, "POST", "/query", 200, 1, 2, ctx, "worker", "s1", "req-1"));
  REQUIRE_NOTHROW(JaegerSpanLogger::log_proxy_response(
      nullptr, "POST", "/query", 200, 1, 2, ctx, "proxy", "req-1"));
  REQUIRE(JaegerSpanLogger::generate_span_id(nullptr) == "");
  REQUIRE_NOTHROW(JaegerSpanLogger::log_nats_span(
      nullptr, "poll", 200, "req-1", "trace-1", "span-1", "parent-1", 1000));
}

TEST_CASE("Tracing helpers: BackendErrorSpanLogger no-op with null tracer "
          "(empty and non-empty detail)",
          "[tracing-helpers]") {
  TraceContext ctx;
  ctx.m_trace_id = "t";
  ctx.m_span_id = "s";
  REQUIRE_NOTHROW(BackendErrorSpanLogger::log_backend_error(
      nullptr, "POST", "/query", 500, 1'000, ctx, "proxy", "req-1",
      "backend_error", ""));
  REQUIRE_NOTHROW(BackendErrorSpanLogger::log_backend_error(
      nullptr, "POST", "/query", 500, 1'000, ctx, "proxy", "req-1",
      "backend_error", "db down"));
}

TEST_CASE("Tracing helpers: RateLimitSpanLogger no-op with null tracer "
          "(empty and non-empty limit/remaining)",
          "[tracing-helpers]") {
  REQUIRE_NOTHROW(RateLimitSpanLogger::log_rate_limit_rejection(
      nullptr, "too many", "1.2.3.4", "", "", ""));
  REQUIRE_NOTHROW(RateLimitSpanLogger::log_rate_limit_rejection(
      nullptr, "too many", "1.2.3.4", "00-aa-bb-01", "10", "0"));
}

TEST_CASE("Tracing helpers: log_incoming_span returns empty with null tracer",
          "[tracing-helpers]") {
  TraceContext ctx;
  ctx.m_trace_id = "trace-1";
  ctx.m_parent_id = "parent-1";
  REQUIRE(log_incoming_span(nullptr, "/query", 1'000, "req-1", ctx) == "");
}

TEST_CASE("Tracing helpers: add_proxy_trace_fields is a no-op with null tracer",
          "[tracing-helpers]") {
  json request_data = nlohmann::json::object();
  TraceContext ctx;
  ctx.m_trace_id = "trace-1";
  request_data[NatsContract::kProxyTraceId] = "";
  add_proxy_trace_fields(request_data, nullptr, ctx, "", "span-1");
  REQUIRE(request_data[NatsContract::kProxyTraceId] == "");
}

TEST_CASE("Tracing helpers: log_worker_span no-ops on null tracer or empty "
          "trace_id",
          "[tracing-helpers]") {
  REQUIRE_NOTHROW(log_worker_span(nullptr, "POST", "/query", 200, 1, 2,
                                  "worker", "req-1", "", "span-1", "parent-1"));
  REQUIRE_NOTHROW(log_worker_span(nullptr, "POST", "/query", 200, 1, 2,
                                  "worker", "req-1", "trace-1", "span-1",
                                  "parent-1"));
  REQUIRE_NOTHROW(log_worker_span(nullptr, "POST", "/query", 200, 1, 2,
                                  "worker", "req-1", "", "", ""));
}

TEST_CASE("Tracing helpers: resolve_trace_id generates when trace id empty and "
          "tracer null returns empty",
          "[tracing-helpers]") {
  TraceContext empty;
  empty.m_trace_id = "";
  empty.m_span_id = "";
  empty.m_parent_id = "";
  REQUIRE(resolve_trace_id(nullptr, empty) == "");
}

// ============================================================================
// db_query_executor_base.cpp — set_db_pool_gauges
// ============================================================================

namespace {

class BasePoolExecMock final : public DbExecutorBase {
public:
  explicit BasePoolExecMock(DbConfig db) : DbExecutorBase(std::move(db)) {}
  bool init() override { return true; }
  [[nodiscard]] bool is_ready() const override {
    return DbExecutorBase::is_ready();
  }
  json execute_query(const std::string &, const json &, int, int,
                     int &) override {
    return json::object();
  }
  bool ping(int) override { return true; }
  void set_error_metrics(prometheus::Family<prometheus::Counter> *errors) override {
    DbExecutorBase::set_error_metrics(errors);
  }
  void set_pool_metrics(prometheus::Family<prometheus::Gauge> *pm) override {
    DbExecutorBase::set_pool_metrics(pm);
  }

protected:
  // Satisfy the pure-virtual contract; the pool is external to the base class.
  void refresh_pool_gauges() override {}
};

} // namespace

TEST_CASE("DbExecutorBase: default getters read DbConfig",
          "[db-executor-base]") {
  DbConfig cfg;
  cfg.m_name = "oracle";
  cfg.m_query_timeout_ms = 3000;
  cfg.m_max_rows = 500;
  BasePoolExecMock const exec(cfg);
  REQUIRE(exec.default_timeout_ms() == 3000);
  REQUIRE(exec.default_max_rows() == 500);
  REQUIRE(exec.db_name() == "oracle");
}

TEST_CASE("DbExecutorBase: set_db_pool_gauges is a no-op before metrics set",
          "[db-executor-base]") {
  DbConfig cfg;
  cfg.m_name = "oracle";
  cfg.m_query_timeout_ms = 1000;
  cfg.m_max_rows = 100;
  BasePoolExecMock exec(cfg);
  REQUIRE_NOTHROW(exec.set_db_pool_gauges(2.0, 3.0));
}

TEST_CASE("DbExecutorBase: set_db_pool_gauges publishes idle/active gauges",
          "[db-executor-base]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &family = test_support::make_gauge_family(
      *registry, "db_pool", "db pool state");
  DbConfig cfg;
  cfg.m_name = "pg";
  cfg.m_query_timeout_ms = 1000;
  cfg.m_max_rows = 100;
  BasePoolExecMock exec(cfg);
  exec.set_pool_metrics(&family);
  exec.set_db_pool_gauges(5.0, 2.0);
  const auto collected = family.Collect();
  REQUIRE(collected.front().metric.size() == 2);
  double idle = -1.0, active = -1.0;
  for (const auto &m : collected.front().metric) {
    for (const auto &label : m.label) {
      if (label.name == "state" && label.value == "idle") {
        idle = m.gauge.value;
      }
      if (label.name == "state" && label.value == "active") {
        active = m.gauge.value;
      }
    }
  }
  REQUIRE(idle == 5.0);
  REQUIRE(active == 2.0);
}

// ============================================================================
// logger.hpp — set_level_from_string additional branches
// ============================================================================

TEST_CASE("Logger: set_level_from_string covers lowercase and alias variants",
          "[logger]") {
  Logger::set_level_from_string("warn");
  REQUIRE(Logger::get_level() == Logger::WARN);
  Logger::set_level_from_string("WARNING");
  REQUIRE(Logger::get_level() == Logger::WARN);
  Logger::set_level_from_string("info");
  REQUIRE(Logger::get_level() == Logger::INFO);
  Logger::set_level_from_string("debug");
  REQUIRE(Logger::get_level() == Logger::DEBUG);
  Logger::set_level_from_string("error");
  REQUIRE(Logger::get_level() == Logger::ERROR);
  Logger::set_level_from_string("UNKNOWN");
  REQUIRE(Logger::get_level() == Logger::INFO);
  Logger::set_level(Logger::DEBUG);
}

// ============================================================================
// rate_limiter_per_ip.hpp — get_per_ip_stats kMaxExpose cap
// ============================================================================

TEST_CASE("PerIPRateLimiter: get_per_ip_stats covers >1000 IPs capped at 1000",
          "[rate-limiter-per-ip-ext]") {
  PerIPRateLimiter limiter(10, 5, 5000, 3600);
  for (int i = 0; i < 1500; ++i) {
    const std::string ip =
        "10.0.0." + std::to_string(i % 250) + "." + std::to_string(i);
    limiter.acquire(ip);
  }
  const auto stats = limiter.get_per_ip_stats();
  REQUIRE(stats.size() == 1000);
  REQUIRE(stats.size() >= 1000);
}

TEST_CASE("PerIPRateLimiter: get_per_ip_stats emits counters for each entry",
          "[rate-limiter-per-ip-ext]") {
  PerIPRateLimiter limiter(10, 5, 5000, 3600);
  limiter.acquire("10.0.0.1");
  limiter.acquire("10.0.0.1");
  const auto stats = limiter.get_per_ip_stats();
  REQUIRE(stats.size() == 1);
  REQUIRE(stats[0].second.m_requests == 2);
}

// ============================================================================
// tracing_helpers.hpp — non-null JaegerLogger paths
// ============================================================================

namespace {

struct JaegerTracerFixture {
  std::shared_ptr<prometheus::Registry> m_registry;
  prometheus::Counter &m_spans_sent;
  prometheus::Counter &m_spans_failed;
  prometheus::Gauge &m_queue_size;
  prometheus::Gauge &m_last_send_duration;
  prometheus::Histogram &m_send_latency;
  prometheus::Histogram &m_queue_time;
  prometheus::Family<prometheus::Counter> &m_spans_dropped;
  std::unique_ptr<JaegerLogger> m_tracer;

  JaegerTracerFixture()
      : m_registry(std::make_shared<prometheus::Registry>()),
        m_spans_sent(test_support::make_counter(
            *m_registry, "test_j_spans_sent", "sent")),
        m_spans_failed(test_support::make_counter(
            *m_registry, "test_j_spans_failed", "failed")),
        m_queue_size(test_support::make_gauge(
            *m_registry, "test_j_queue_size", "queue")),
        m_last_send_duration(test_support::make_gauge(
            *m_registry, "test_j_last_send_duration", "dur")),
        m_send_latency(test_support::make_histogram(
            *m_registry, "test_j_send_latency", "lat",
            std::vector<double>{0.001, 0.01, 0.1, 1.0})),
        m_queue_time(test_support::make_histogram(
            *m_registry, "test_j_queue_time", "qt",
            std::vector<double>{0.001, 0.01, 0.1, 1.0})),
        m_spans_dropped(test_support::make_counter_family(
            *m_registry, "test_j_spans_dropped", "dropped")) {
    m_tracer = std::make_unique<JaegerLogger>(
        "http://localhost:19999",
        JaegerDeliveryMetrics{m_spans_sent, m_spans_failed, m_queue_size,
                               m_last_send_duration, m_send_latency,
                               m_queue_time, m_spans_dropped},
        /*batch_size=*/2, /*flush_interval_ms=*/100, /*sample_rate=*/1.0);
  }
};

} // namespace

TEST_CASE("Tracing helpers: resolve_trace_id generates when empty with real "
          "tracer",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  TraceContext empty;
  empty.m_trace_id = "";
  const auto tid = resolve_trace_id(fix.m_tracer.get(), empty);
  REQUIRE_FALSE(tid.empty());
  REQUIRE(tid.size() == 32);
}

TEST_CASE("Tracing helpers: resolve_trace_id returns existing when non-empty",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  TraceContext ctx;
  ctx.m_trace_id = "known-trace-id";
  REQUIRE(resolve_trace_id(fix.m_tracer.get(), ctx) == "known-trace-id");
}

TEST_CASE("Tracing helpers: log_incoming_span returns non-empty span_id with "
          "real tracer",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  TraceContext ctx;
  ctx.m_trace_id = "";
  ctx.m_parent_id = "parent-abc";
  const auto span_id =
      log_incoming_span(fix.m_tracer.get(), "/v1/sql/oracle/query",
                        TimeUtils::epoch_us(), "req-123", ctx);
  REQUIRE_FALSE(span_id.empty());
  REQUIRE(span_id.size() == 16);
}

TEST_CASE("Tracing helpers: log_incoming_span with pre-existing trace_id",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  TraceContext ctx;
  ctx.m_trace_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  ctx.m_parent_id = "";
  const auto span_id = log_incoming_span(fix.m_tracer.get(), "/api/x",
                                         1'000'000, "req-456", ctx);
  REQUIRE_FALSE(span_id.empty());
}

TEST_CASE("Tracing helpers: make_span_and_traceparent generates traceparent "
          "with real tracer",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  TraceContext ctx;
  ctx.m_trace_id = "1234567890abcdef1234567890abcdef";
  ctx.m_traceparent_header = "";
  const auto [span_id, tp] =
      make_span_and_traceparent(fix.m_tracer.get(), ctx, "custom-span");
  REQUIRE(span_id == "custom-span");
  REQUIRE_FALSE(tp.empty());
  REQUIRE(tp.substr(0, 3) == "00-");
  REQUIRE(tp.find("1234567890abcdef1234567890abcdef") != std::string::npos);
  REQUIRE(tp.find("custom-span") != std::string::npos);
}

TEST_CASE("Tracing helpers: make_span_and_traceparent generates span_id when "
          "hint is empty",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  TraceContext ctx;
  ctx.m_trace_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const auto [span_id, tp] = make_span_and_traceparent(fix.m_tracer.get(), ctx);
  REQUIRE_FALSE(span_id.empty());
  REQUIRE(span_id.size() == 16);
  REQUIRE_FALSE(tp.empty());
}

TEST_CASE("Tracing helpers: make_span_and_traceparent returns header when "
          "trace_id is empty",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  TraceContext ctx;
  ctx.m_trace_id = "";
  ctx.m_traceparent_header = "00-aabb-ccdd-01";
  const auto [span_id, tp] = make_span_and_traceparent(fix.m_tracer.get(), ctx);
  REQUIRE(tp == "00-aabb-ccdd-01");
}

TEST_CASE("Tracing helpers: add_proxy_trace_fields fills JSON with real tracer",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  json request_data = nlohmann::json::object();
  TraceContext ctx;
  ctx.m_trace_id = "11111111111111111111111111111111";
  add_proxy_trace_fields(request_data, fix.m_tracer.get(), ctx, "inlet-span-1",
                         "proxy-span-1");
  REQUIRE(request_data.contains(NatsContract::kProxyTraceId));
  REQUIRE(request_data.contains(NatsContract::kProxySpanId));
  REQUIRE(request_data.contains(NatsContract::kProxyInletSpanId));
  REQUIRE(request_data.contains(NatsContract::kProxyTraceparent));
  REQUIRE(request_data[NatsContract::kProxyTraceId] ==
          "11111111111111111111111111111111");
  REQUIRE(request_data[NatsContract::kProxySpanId] == "proxy-span-1");
  REQUIRE(request_data[NatsContract::kProxyInletSpanId] == "inlet-span-1");
  const std::string tp =
      request_data[NatsContract::kProxyTraceparent].get<std::string>();
  REQUIRE(tp.substr(0, 3) == "00-");
  REQUIRE(tp.find("11111111111111111111111111111111") != std::string::npos);
  REQUIRE(tp.find("proxy-span-1") != std::string::npos);
}

TEST_CASE("Tracing helpers: add_proxy_trace_fields resolves trace_id from "
          "empty ctx",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  json request_data = nlohmann::json::object();
  TraceContext ctx;
  ctx.m_trace_id = "";
  add_proxy_trace_fields(request_data, fix.m_tracer.get(), ctx, "", "span-2");
  const std::string resolved_tid =
      request_data[NatsContract::kProxyTraceId].get<std::string>();
  REQUIRE_FALSE(resolved_tid.empty());
  REQUIRE(resolved_tid.size() == 32);
}

TEST_CASE("Tracing helpers: JaegerSpanLogger non-null tracer paths",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  TraceContext ctx;
  ctx.m_trace_id = "22222222222222222222222222222222";
  ctx.m_span_id = "2222222222222222";
  ctx.m_parent_id = "3333333333333333";

  const auto now = TimeUtils::epoch_us();
  JaegerSpanLogger::log_l2_call(fix.m_tracer.get(), "POST",
                                "/v1/sql/oracle/query", 200, now, now + 5000,
                                ctx, "proxy", "sp-l2", "par-l2", "req-l2");
  JaegerSpanLogger::log_worker_processing(
      fix.m_tracer.get(), "POST", "/v1/sql/oracle/query", 200, now, now + 3000,
      ctx, "worker", "sp-wrk", "req-wrk");
  JaegerSpanLogger::log_proxy_response(fix.m_tracer.get(), "POST",
                                       "/v1/sql/oracle/query", 200, now,
                                       now + 4000, ctx, "proxy", "req-pr");
  JaegerSpanLogger::log_nats_span(fix.m_tracer.get(), "poll", 200, "req-nat",
                                  "22222222222222222222222222222222",
                                  "span-nat", "parent-nat", now);
  const auto span_id = JaegerSpanLogger::generate_span_id(fix.m_tracer.get());
  REQUIRE_FALSE(span_id.empty());
  REQUIRE(span_id.size() == 16);
}

TEST_CASE("Tracing helpers: BackendErrorSpanLogger non-null tracer with "
          "empty and non-empty detail",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  TraceContext ctx;
  ctx.m_trace_id = "44444444444444444444444444444444";
  ctx.m_span_id = "4444444444444444";
  ctx.m_parent_id = "5555555555555555";
  const auto now = TimeUtils::epoch_us();

  BackendErrorSpanLogger::log_backend_error(
      fix.m_tracer.get(), "POST", "/v1/sql/oracle/query", 500, now, ctx,
      "proxy", "req-err", "backend_error", "");
  BackendErrorSpanLogger::log_backend_error(
      fix.m_tracer.get(), "POST", "/v1/sql/oracle/query", 502, now, ctx,
      "proxy", "req-err2", "timeout", "worker did not respond");
}

TEST_CASE("Tracing helpers: RateLimitSpanLogger non-null tracer",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;

  RateLimitSpanLogger::log_rate_limit_rejection(
      fix.m_tracer.get(), "too many requests", "10.0.0.1", "", "10", "0");
  RateLimitSpanLogger::log_rate_limit_rejection(
      fix.m_tracer.get(), "burst", "10.0.0.2",
      "00-66666666666666666666666666666666-aaaa-01", "100", "50");
}

TEST_CASE("Tracing helpers: log_worker_span with real tracer and non-empty "
          "trace_id",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  const auto now = TimeUtils::epoch_us();
  REQUIRE_NOTHROW(log_worker_span(
      fix.m_tracer.get(), "POST", "/query", 200, now, now + 1000, "worker",
      "req-ws", "77777777777777777777777777777777", "span-ws", "parent-ws"));
}

TEST_CASE("Tracing helpers: log_worker_span no-ops with empty trace_id even "
          "with real tracer",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  REQUIRE_NOTHROW(log_worker_span(fix.m_tracer.get(), "POST", "/query", 200, 1,
                                  2, "worker", "req-ws2", "", "", ""));
}

TEST_CASE("Tracing helpers: TraceContextHelper extract_from_raw with "
          "non-empty traceparent and real tracer",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  const TraceContext ctx = TraceContextHelper::extract_from_raw(
      "00-88888888888888888888888888888888-aaaaaaaaaaaaaaaa-01",
      fix.m_tracer.get(), "test-ctx");
  REQUIRE(ctx.m_trace_id == "88888888888888888888888888888888");
  REQUIRE(ctx.m_parent_id == "aaaaaaaaaaaaaaaa");
  REQUIRE_FALSE(ctx.m_span_id.empty());
  REQUIRE(ctx.m_sampled);
}

TEST_CASE("Tracing helpers: TraceContextHelper extract_from_raw with "
          "empty traceparent and real tracer",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  const TraceContext ctx = TraceContextHelper::extract_from_raw(
      "", fix.m_tracer.get(), "test-empty");
  REQUIRE_FALSE(ctx.m_trace_id.empty());
  REQUIRE_FALSE(ctx.m_span_id.empty());
  REQUIRE(ctx.m_parent_id.empty());
}

TEST_CASE("Tracing helpers: TraceContextHelper extract_and_validate with "
          "headers",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  httplib::Headers hdrs;
  hdrs.emplace("traceparent",
               "00-99999999999999999999999999999999-bbbbbbbbbbbbbbbb-01");
  const TraceContext ctx = TraceContextHelper::extract_and_validate(
      hdrs, fix.m_tracer.get(), "test-validate");
  REQUIRE(ctx.m_trace_id == "99999999999999999999999999999999");
  REQUIRE(ctx.m_parent_id == "bbbbbbbbbbbbbbbb");
  REQUIRE_FALSE(ctx.m_span_id.empty());
}

TEST_CASE("Tracing helpers: begin_request_trace with real tracer",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  httplib::Headers hdrs;
  hdrs.emplace("traceparent",
               "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-cccccccccccccccc-01");
  std::string inlet_span_id;
  const TraceContext ctx = begin_request_trace(
      fix.m_tracer.get(), hdrs, "req-brt", "/v1/sql/oracle/query",
      TimeUtils::epoch_us(), inlet_span_id);
  REQUIRE(ctx.m_trace_id == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  REQUIRE_FALSE(inlet_span_id.empty());
  REQUIRE(inlet_span_id.size() == 16);
}

TEST_CASE("Tracing helpers: begin_request_trace generates trace_id when "
          "no traceparent",
          "[tracing-helpers][non-null]") {
  JaegerTracerFixture const fix;
  httplib::Headers const hdrs;
  std::string inlet_span_id;
  const TraceContext ctx =
      begin_request_trace(fix.m_tracer.get(), hdrs, "req-brt2", "/ping",
                          TimeUtils::epoch_us(), inlet_span_id);
  REQUIRE(ctx.m_trace_id.size() == 32);
  REQUIRE_FALSE(inlet_span_id.empty());
}

// ============================================================================
// Branch coverage: common_utils.cpp — parse_url
// ============================================================================

TEST_CASE("parse_url: throws on empty string", "[parse-url][branch-cover]") {
  REQUIRE_THROWS_AS(parse_url(""), std::runtime_error);
}

TEST_CASE("parse_url: http URL with port and path",
          "[parse-url][branch-cover]") {
  auto p = parse_url("http://example.com:8080/api/v1");
  REQUIRE(p.m_host == "example.com");
  REQUIRE(p.m_port == 8080);
  REQUIRE(p.m_path == "/api/v1");
  REQUIRE_FALSE(p.m_is_https);
}

TEST_CASE("parse_url: https URL without port", "[parse-url][branch-cover]") {
  auto p = parse_url("https://example.com/data");
  REQUIRE(p.m_host == "example.com");
  REQUIRE(p.m_port == 443);
  REQUIRE(p.m_path == "/data");
  REQUIRE(p.m_is_https);
}

TEST_CASE("parse_url: URL with host only (no path after host)",
          "[parse-url][branch-cover]") {
  auto p = parse_url("http://example.com");
  REQUIRE(p.m_host == "example.com");
  REQUIRE(p.m_path == "/");
}

TEST_CASE("parse_url: URL with port but no path", "[parse-url][branch-cover]") {
  auto p = parse_url("https://example.com:9443");
  REQUIRE(p.m_host == "example.com");
  REQUIRE(p.m_port == 9443);
  REQUIRE(p.m_path == "/");
}

TEST_CASE("parse_url: no-protocol URL throws (empty host)",
          "[parse-url][branch-cover]") {
  REQUIRE_THROWS_AS(parse_url("/v1/sql/oracle"), std::runtime_error);
}

TEST_CASE("parse_url: throws on protocol-only URL",
          "[parse-url][branch-cover]") {
  REQUIRE_THROWS_AS(parse_url("https://"), std::runtime_error);
}

TEST_CASE("parse_url: throws on unparseable port",
          "[parse-url][branch-cover]") {
  auto p = parse_url("http://host:notaport/path");
  REQUIRE(p.m_host == "host");
  REQUIRE(p.m_port == 80);
}

// ============================================================================
// Branch coverage: common_utils.cpp — format_http_error
// ============================================================================

TEST_CASE("format_http_error: Read error includes timeout",
          "[format-http-error][branch-cover]") {
  auto msg = format_http_error(httplib::Error::Read, 30, "GET /api");
  REQUIRE(msg.find("timeout after 30") != std::string::npos);
}

TEST_CASE("format_http_error: Write error includes timeout",
          "[format-http-error][branch-cover]") {
  auto msg = format_http_error(httplib::Error::Write, 15, "POST /api");
  REQUIRE(msg.find("timeout after 15") != std::string::npos);
}

TEST_CASE("format_http_error: Connection error",
          "[format-http-error][branch-cover]") {
  auto msg = format_http_error(httplib::Error::Connection, 10, "GET /api");
  REQUIRE(msg.find("connection failed") != std::string::npos);
}

TEST_CASE("format_http_error: BindIPAddress error",
          "[format-http-error][branch-cover]") {
  auto msg = format_http_error(httplib::Error::BindIPAddress, 5, "GET /api");
  REQUIRE(msg.find("failed to bind IP") != std::string::npos);
}
TEST_CASE("format_http_error: other error (no suffix)",
          "[format-http-error][branch-cover]") {
  auto msg =
      format_http_error(httplib::Error::Unknown, 10, "GET /api");
  REQUIRE(msg.find("failed:") != std::string::npos);
  REQUIRE(msg.find("timeout") == std::string::npos);
  REQUIRE(msg.find("connection failed") == std::string::npos);
}

// ============================================================================
// timed_task_queue.hpp
// ============================================================================

namespace {

struct TaskQueueFixture {
  std::shared_ptr<prometheus::Registry> m_registry =
      std::make_shared<prometheus::Registry>();
  prometheus::Counter &m_enqueued =
      MetricsManager::create_counter(m_registry, "queue_enqueued_total",
                                     "accepted tasks");
  prometheus::Counter &m_rejected =
      MetricsManager::create_counter(m_registry, "queue_rejected_total",
                                     "rejected tasks");
  prometheus::Histogram &m_wait =
      MetricsManager::create_histogram(m_registry, "queue_wait_seconds",
                                       "queue wait", latency_buckets_ms_to_10s());
};

// The timed-task-queue tests watch an atomic flag that flips within
// microseconds, so they poll the shared waiter every 1 ms instead of its 50 ms
// default.
bool wait_for_tasks(const std::atomic<int> &done, int expected,
                    std::chrono::milliseconds timeout) {
  return test_wait_support::wait_for_condition(
      [&] { return done.load() >= expected; },
      static_cast<int>(timeout.count()), std::chrono::milliseconds(1));
}

bool wait_for_flag(std::mutex &mutex, bool &flag,
                   std::chrono::milliseconds timeout) {
  return test_wait_support::wait_for_condition(
      [&] {
        std::scoped_lock const lock(mutex);
        return flag;
      },
      static_cast<int>(timeout.count()), std::chrono::milliseconds(1));
}

} // namespace

TEST_CASE("TimedTaskQueue: counts accepted tasks and observes their wait",
          "[timed-task-queue]") {
  TaskQueueFixture fixture;
  TimedTaskQueue queue(fixture.m_enqueued, fixture.m_rejected, fixture.m_wait, 2);

  std::atomic<int> done{0};
  constexpr int kTasks = 6;
  for (int i = 0; i < kTasks; ++i) {
    REQUIRE(queue.enqueue([&done]() { done.fetch_add(1); }));
  }
  REQUIRE(wait_for_tasks(done, kTasks, std::chrono::seconds(5)));
  queue.on_idle();
  queue.shutdown();

  REQUIRE(fixture.m_enqueued.Value() == static_cast<double>(kTasks));
  REQUIRE(fixture.m_rejected.Value() == 0.0);
  const auto collected = fixture.m_wait.Collect();
  REQUIRE(collected.histogram.sample_count == static_cast<std::uint64_t>(kTasks));
  REQUIRE(collected.histogram.sample_sum >= 0.0);
}

TEST_CASE("TimedTaskQueue: counts rejections when the queue is full",
          "[timed-task-queue]") {
  TaskQueueFixture fixture;
  TimedTaskQueue queue(fixture.m_enqueued, fixture.m_rejected, fixture.m_wait, 1,
                       1, 1);

  std::mutex gate;
  std::condition_variable gate_cv;
  bool started = false;
  bool release = false;
  std::atomic<int> done{0};

  // False positive: the analyzer loses the std::function allocation inside
  // the Catch2 macro.
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
  REQUIRE(queue.enqueue([&]() {
    {
      std::scoped_lock const lock(gate);
      started = true;
    }
    gate_cv.notify_all();
    std::unique_lock lock(gate);
    gate_cv.wait(lock, [&release]() { return release; });
    done.fetch_add(1);
  }));
  REQUIRE(wait_for_flag(gate, started, std::chrono::seconds(5)));
  REQUIRE(queue.enqueue([&done]() { done.fetch_add(1); }));
  REQUIRE_FALSE(queue.enqueue([&done]() { done.fetch_add(1); }));

  {
    std::scoped_lock const lock(gate);
    release = true;
  }
  gate_cv.notify_all();
  REQUIRE(wait_for_tasks(done, 2, std::chrono::seconds(5)));
  queue.shutdown();

  REQUIRE(fixture.m_enqueued.Value() == 2.0);
  REQUIRE(fixture.m_rejected.Value() == 1.0);
  REQUIRE(fixture.m_wait.Collect().histogram.sample_count == 2);
}

// Unit tests closing coverage gaps in the remaining header-only and small
// .cpp modules: exceptions, ScopedMetrics, ScopedProfiler, pool_executor,
// metrics_manager, tracing_helpers, stats_page, trace_context_extractor and
// ScopedRequestContext. These are dependency-light (no NATS / DB) and are
// compiled into the test_components target alongside the existing suites.

#include "common_utils.hpp"
#include "crash_handler.hpp"
#include "exceptions.hpp"
#include "metrics_manager.hpp"
#include "pool_executor.hpp"
#include "scoped_metrics.hpp"
#include "scoped_profiler.hpp"
#include "stats_page.hpp"
#include "string_utils.hpp"
#include "thread_pool_wrapper.hpp"
#include "trace_context_extractor.hpp"
#include "tracing_helpers.hpp"
#include "url_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "httplib/httplib.h"
#include "prometheus/summary.h"
#include <chrono>
#include <memory>
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

TEST_CASE("Exceptions: L2ProxyException derives from runtime_error",
          "[exceptions]") {
  L2ProxyException ex("boom");
  REQUIRE_THROWS_AS(throw ex, std::runtime_error);
  REQUIRE(std::string(ex.what()) == "boom");
}

TEST_CASE("Exceptions: TimeoutException prefixes the message",
          "[exceptions]") {
  TimeoutException ex("read failed");
  REQUIRE_THROWS_AS(throw ex, L2ProxyException);
  REQUIRE_THROWS_AS(throw ex, std::runtime_error);
  REQUIRE(std::string(ex.what()) == "Timeout error: read failed");
}

// ============================================================================
// scoped_metrics.hpp
// ============================================================================

TEST_CASE("ScopedMetrics: increments the counter on scope exit",
          "[scoped-metrics]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &counter = prometheus::BuildCounter()
                      .Name("scoped_count")
                      .Help("scoped test")
                      .Register(*registry)
                      .Add({});
  {
    ScopedMetrics guard(counter);
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
  auto &hist = prometheus::BuildHistogram()
                   .Name("scoped_prof")
                   .Help("scoped test")
                   .Register(*registry)
                   .Add({}, std::vector<double>{0.001, 1.0});
  {
    ScopedProfiler profiler(hist);
    REQUIRE(hist.Collect().histogram.sample_count == 0);
  }
  const auto collected = hist.Collect();
  REQUIRE(collected.histogram.sample_count == 1);
  REQUIRE(collected.histogram.sample_sum > 0.0);
}

TEST_CASE("ScopedLabeledProfiler: null collector is a no-op",
          "[scoped-profiler]") {
  ScopedLabeledProfiler profiler(nullptr, "client-1");
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
    ScopedLabeledProfiler profiler(&family, "alice");
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

  bool is_valid() const { return m_valid; }
  int get_last_status_code() const { return m_last_status; }
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
    std::unique_ptr<MockHttpClient> acquire_connection() {
      auto c = std::make_unique<MockHttpClient>();
      c->m_valid = false;
      return c;
    }
    void release_connection(std::unique_ptr<MockHttpClient>) { ++m_releases; }
  };
  InvalidPool pool;
  REQUIRE_THROWS_AS(
      execute_http_command_with_status(&pool, [](MockHttpClient *) {
        return std::string("never reached");
      }),
      std::runtime_error);
  REQUIRE(pool.m_releases == 1);
}

TEST_CASE("Pool executor: exception in func invalidates and releases then "
          "rethrows", "[pool-executor]") {
  MockPool pool;
  REQUIRE_THROWS_AS(
      execute_http_command_with_status(&pool, [](MockHttpClient *) -> int {
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
  auto &hist = MetricsManager::create_histogram(
      registry, "mm_hist", "mm", std::vector<double>{0.5, 1.0});
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
      registry, "mm_hfam", "", std::vector<double>{0.1, 1.0});
  hfam.Add({{"db", "x"}}, std::vector<double>{0.1, 1.0}).Observe(0.2);
  auto hcollected = hfam.Collect();
  REQUIRE(hcollected.front().metric.size() == 1);
  REQUIRE(hcollected.front().metric[0].histogram.sample_count == 1);
}

TEST_CASE("MetricsManager: array-arg creators forward bucket bounds",
          "[metrics-manager]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &hist = MetricsManager::create_histogram(
      registry, "mm_hist_arr", "", histogram_buckets::g_k_latency_ms_to_10s);
  hist.Observe(5.0);
  auto &fam = MetricsManager::create_histogram_family(
      registry, "mm_hfam_arr", "", histogram_buckets::g_k_latency_ms_to_5s);
  REQUIRE(fam.Collect().empty());
}

TEST_CASE("MetricsManager: record_db_request_metrics increments labelled "
          "series", "[metrics-manager]") {
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
      registry, "db_dur", "", latency_buckets_ms_to_10s());
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
  TraceContext empty;
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

TEST_CASE("Tracing helpers: set_traceparent_response_header sets when non-empty",
          "[tracing-helpers]") {
  httplib::Response res;
  TraceContext empty;
  set_traceparent_response_header(res, empty);
  REQUIRE(res.get_header_value("traceparent") == "");

  TraceContext ctx;
  ctx.m_traceparent_header = "00-a-b-01";
  set_traceparent_response_header(res, ctx);
  REQUIRE(res.get_header_value("traceparent") == "00-a-b-01");
}

TEST_CASE("Tracing helpers: begin_request_trace with null tracer",
          "[tracing-helpers]") {
  httplib::Headers headers;
  std::string inlet;
  const TraceContext ctx =
      begin_request_trace(nullptr, headers, "req-1", "/p", 1234, inlet);
  REQUIRE(ctx.m_trace_id == "");
  REQUIRE(ctx.m_span_id == "");
  REQUIRE(inlet == "");
}

TEST_CASE("Tracing helpers: TraceContextHelper extract_from_raw with null tracer",
          "[tracing-helpers]") {
  const TraceContext ctx =
      TraceContextHelper::extract_from_raw("", nullptr, "test");
  REQUIRE(ctx.m_trace_id == "");
}

TEST_CASE("Tracing helpers: make_span_and_traceparent with traced context and "
          "hint", "[tracing-helpers]") {
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
  const std::string h = format_metric_value(hist, prometheus::MetricType::Histogram);
  REQUIRE(h.find("count=5") != std::string::npos);
  REQUIRE(h.find("sum=2.5") != std::string::npos);

  prometheus::ClientMetric summary;
  summary.summary.sample_count = 7;
  summary.summary.sample_sum = 1.0;
  const std::string s = format_metric_value(summary, prometheus::MetricType::Summary);
  REQUIRE(s.find("count=7") != std::string::npos);
  REQUIRE(s.find("sum=1") != std::string::npos);
}

TEST_CASE("Stats page: format_labels renders braces", "[stats-page-ext]") {
  REQUIRE(format_labels({}) == "");
  const std::vector<prometheus::ClientMetric::Label> labels = {
      {"job", "proxy"}, {"state", "ok"}};
  REQUIRE(format_labels(labels) == "{job=proxy, state=ok}");
}

TEST_CASE("Stats page: build_stats_html renders banner and tiles",
          "[stats-page-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &gauge = MetricsManager::create_gauge(registry, "health_ready", "ready");
  gauge.Set(1.0);
  auto &counter = prometheus::BuildCounter()
                      .Name("requests_total")
                      .Help("requests")
                      .Register(*registry)
                      .Add({});
  counter.Increment();

  const std::string html = build_stats_html("proxy-test", registry, nullptr, 30);
  REQUIRE(html.find("OPERATIONAL") != std::string::npos);
  REQUIRE(html.find("proxy-test") != std::string::npos);
  REQUIRE(html.find("health_ready") != std::string::npos);
  REQUIRE(html.find("requests_total") != std::string::npos);
}

TEST_CASE("Stats page: build_stats_html flags degraded when health not ready",
          "[stats-page-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &gauge = MetricsManager::create_gauge(registry, "health_ready", "ready");
  gauge.Set(0.0);
  const std::string html = build_stats_html("proxy-test", registry, nullptr, 30);
  REQUIRE(html.find("DEGRADED") != std::string::npos);
}

TEST_CASE("Stats page: escape_html escapes special characters",
          "[stats-page-ext]") {
  REQUIRE(escape_html("a&b") == "a&amp;b");
  REQUIRE(escape_html("<tag>") == "&lt;tag&gt;");
  REQUIRE(escape_html("say \"hi\"") == "say &quot;hi&quot;");
  REQUIRE(escape_html("plain") == "plain");
}

TEST_CASE("Stats page: parse_stats_window handles absent/invalid/clamped values",
          "[stats-page-ext]") {
  std::map<std::string, std::string> no_window;
  REQUIRE(parse_stats_window(no_window) == 30);
  REQUIRE(parse_stats_window(no_window, 60) == 60);

  std::map<std::string, std::string> exact{{"window", "15"}};
  REQUIRE(parse_stats_window(exact) == 15);

  std::map<std::string, std::string> invalid{{"window", "abc"}};
  REQUIRE(parse_stats_window(invalid) == 30);

  std::map<std::string, std::string> clamped_high{{"window", "999"}};
  REQUIRE(parse_stats_window(clamped_high) == 120);

  std::map<std::string, std::string> clamped_low{{"window", "0"}};
  REQUIRE(parse_stats_window(clamped_low) == 1);

  std::map<std::string, std::string> partial{{"window", "7x"}};
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

TEST_CASE("Stats page: build_sparkline_svg renders raw gauge and applies window",
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
  const std::string html = build_stats_html("proxy-test", registry, nullptr, 30);
  REQUIRE(html.find("DEGRADED") != std::string::npos);
}

TEST_CASE("Stats page: build_stats_html caps dense families with more marker",
          "[stats-page-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &family = prometheus::BuildGauge()
                     .Name("per_ip_requests")
                     .Help("per ip")
                     .Register(*registry);
  for (int i = 0; i < 8; ++i) {
    family.Add({{"ip", "10.0.0." + std::to_string(i)}})
        .Set(static_cast<double>(i));
  }
  const std::string html = build_stats_html("proxy-test", registry, nullptr, 30);
  REQUIRE(html.find("+2 more") != std::string::npos);
}

TEST_CASE("Stats page: build_stats_html renders sparklines from history",
          "[stats-page-ext]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &counter = prometheus::BuildCounter()
                      .Name("spark_requests_total")
                      .Help("requests")
                      .Register(*registry)
                      .Add({});
  counter.Increment();
  MetricsHistory history(registry, std::chrono::seconds(1), 8, 240);
  history.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(2300));
  history.stop();
  const std::string html = build_stats_html("proxy-test", registry, &history, 5);
  REQUIRE(html.find("<div class=\"sparkwrap\"") != std::string::npos);
}

TEST_CASE("MetricsHistory: samples registry into a bounded ring buffer",
          "[metrics-history]") {
  auto registry = std::make_shared<prometheus::Registry>();
  auto &cnt = prometheus::BuildCounter()
                  .Name("mh_count")
                  .Help("h")
                  .Register(*registry)
                  .Add({});
  cnt.Increment();
  auto &gauge = prometheus::BuildGauge()
                    .Name("mh_gau")
                    .Help("h")
                    .Register(*registry);
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
  httplib::Request req;
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
  ScopedRequestContext ctx(req);
  REQUIRE(ctx.client_ip() == "unknown");
}

TEST_CASE("ScopedRequestContext: uses x-real-ip when present",
          "[scoped-request-context]") {
  httplib::Request req;
  req.remote_addr = "9.9.9.9";
  req.headers.emplace("x-real-ip", "1.2.3.4");
  ScopedRequestContext ctx(req);
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
  for (size_t i = 0; i < tasks; ++i) {
    results.push_back(
        pool.enqueue([i]() { return static_cast<int>(i * i); }));
  }
  for (size_t i = 0; i < tasks; ++i) {
    REQUIRE(results[i].get() == static_cast<int>(i * i));
  }
}

TEST_CASE("ThreadPoolWrapper: CUSTOM queue_size reflects pending work",
          "[thread-pool-wrapper]") {
  ThreadPoolWrapper pool(ThreadPoolWrapper::Type::CUSTOM, 1, 2);
  pool.enqueue([]() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
  REQUIRE(pool.queue_size() <= 2);
  pool.enqueue([]() {});
  pool.enqueue([]() {});
  REQUIRE(pool.queue_size() <= 2);
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
  auto &recv = prometheus::BuildCounter()
                   .Name("cu_recv")
                   .Help("h")
                   .Register(*registry)
                   .Add({});
  auto &sent = prometheus::BuildCounter()
                   .Name("cu_sent")
                   .Help("h")
                   .Register(*registry)
                   .Add({});
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
  auto &inv = prometheus::BuildCounter()
                  .Name("cu_total")
                  .Help("h")
                  .Register(*registry)
                  .Add({});
  auto &json_err = prometheus::BuildCounter()
                       .Name("cu_json")
                       .Help("h")
                       .Register(*registry)
                       .Add({});
  auto &valid_err = prometheus::BuildCounter()
                        .Name("cu_valid")
                        .Help("h")
                        .Register(*registry)
                        .Add({});
  auto &decomp_err = prometheus::BuildCounter()
                         .Name("cu_decomp")
                         .Help("h")
                         .Register(*registry)
                         .Add({});
  auto &other_err = prometheus::BuildCounter()
                        .Name("cu_other")
                        .Help("h")
                        .Register(*registry)
                        .Add({});
  ProcessingErrorMetrics metrics;
  metrics.m_total_errors = &inv;
  metrics.m_json_errors = &json_err;
  metrics.m_validation_errors = &valid_err;
  metrics.m_decompression_errors = &decomp_err;
  metrics.m_other_errors = &other_err;

  handle_processing_error_with_category("invalid json: syntax error", metrics);
  REQUIRE(inv.Value() == 1.0);
  REQUIRE(json_err.Value() == 1.0);
  REQUIRE(valid_err.Value() == 0.0);
  REQUIRE(decomp_err.Value() == 0.0);
  REQUIRE(other_err.Value() == 0.0);

  handle_processing_error_with_category("bzip2 decompression failed",
                                        metrics);
  REQUIRE(inv.Value() == 2.0);
  REQUIRE(decomp_err.Value() == 1.0);
  REQUIRE(other_err.Value() == 0.0);

  handle_processing_error_with_category(
      "validation failed: unknown field missing", metrics);
  REQUIRE(inv.Value() == 3.0);
  REQUIRE(valid_err.Value() == 1.0);
  REQUIRE(other_err.Value() == 0.0);

  handle_processing_error_with_category("random mystery error", metrics);
  REQUIRE(inv.Value() == 4.0);
  REQUIRE(other_err.Value() == 1.0);
}

TEST_CASE("Common utils ext: handle_processing_error_with_category tolerates "
          "null counters",
          "[common-utils-ext]") {
  ProcessingErrorMetrics metrics;
  REQUIRE_NOTHROW(
      handle_processing_error_with_category("invalid json: boom", metrics));
}

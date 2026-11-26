#include "request_handler.hpp"
#include "app_context.hpp"
#include "constants.hpp"
#include "crash_handler.hpp"
#include "duplicate_detector.hpp"
#include "json_response_utils.hpp"
#include "logger.hpp"
#include "metrics_manager.hpp"
#include "nats_client.hpp" // IWYU pragma: keep (NatsClient via shared_ptr needs complete type)
#include "stats_page.hpp"
#include "url_utils.hpp"
#include <cstddef>
#include <cstdio>
#include <nlohmann/json.hpp>
#if __has_include(<stacktrace>)
#include <stacktrace>
#endif

// Depth of the /debug/stacktrace dump and the widest textual frame address.
constexpr int kMaxBacktraceFrames = 32;
constexpr std::size_t kPointerTextLength = 32;

void RequestHandler::handle_stats(const httplib::Request &req,
                                  httplib::Response &res) {
  // Lightweight HTML status page (no Grafana needed) sourced from the Prometheus
  // registry. Lets operators assess service health directly. Optional ?window=N
  // (minutes, default 30) controls the sparkline lookback.
  res.set_content(build_stats_html("l2-proxy", m_ctx.m_proxy_registry,
                                   m_ctx.m_proxy_stats_history.get(),
                                   parse_stats_window(req.params),
                                   m_ctx.m_common_registry,
                                   m_ctx.m_common_stats_history.get(),
                                   m_ctx.m_stats_page_render_duration),
                  "text/html; charset=utf-8");
}

void RequestHandler::handle_crash_test(const httplib::Request &req,
                                       httplib::Response &res) const {
  // Intentionally crashes the process to test the crash handler. Guarded
  // behind ENABLE_CRASH_TEST_ENDPOINT (default off) so a public client can
  // not remotely SIGSEGV the proxy.
  if (!m_ctx.m_config.m_app.m_enable_crash_test_endpoint) {
    set_json_error_response(
        res, http_status::kNotFound,
        "crash test endpoint is disabled (ENABLE_CRASH_TEST_ENDPOINT=false)");
    return;
  }
  Logger::error("CRASH TEST: intentional SIGSEGV via /crash-test endpoint");
  volatile int *bad_ptr = nullptr;
  // cppcheck-suppress nullPointer
  *bad_ptr = 42; // NOLINT //-V522 triggers SIGSEGV for crash handler test
}

void RequestHandler::handle_stacktrace(httplib::Response &res) {
#if __has_include(<stacktrace>)
  auto trace = std::stacktrace::current();
  nlohmann::json j = nlohmann::json::array();
  for (std::size_t i = 0; i < trace.size(); ++i) {
    const auto &e = trace[i];
    j.push_back({{"index", i},
                 {"description", e.description()},
                 {"source_file", e.source_file()},
                 {"source_line", e.source_line()}});
  }
  CrashHandler::log_current_stacktrace();
  send_json_response(res, http_status::kOk,
                     {{"stacktrace", j}, {"frames", trace.size()}});
#else
  void *callstack[kMaxBacktraceFrames];
  int frames = backtrace(callstack, kMaxBacktraceFrames);
  nlohmann::json j = nlohmann::json::array();
  for (int i = 0; i < frames; ++i) {
    char buf[kPointerTextLength];
    snprintf(buf, sizeof(buf), "%p", callstack[i]);
    j.push_back({{"index", i}, {"address", buf}});
  }
  CrashHandler::log_current_stacktrace();
  send_json_response(res, http_status::kOk,
                     {{"stacktrace", j}, {"frames", frames}});
#endif
}

void RequestHandler::handle_health_ready(httplib::Response &res) {
  readiness::NatsProbe probe;
  readiness::Result result;
  try {
    probe.m_has_client = static_cast<bool>(m_ctx.m_nats_client);
    if (probe.m_has_client) {
      probe.m_allow_connect =
          m_ctx.m_config.m_app.m_health_ready_allow_connect;
      probe.m_connected = m_ctx.m_nats_client->is_connected();
      if (probe.m_allow_connect && probe.m_connected) {
        // Opt-in legacy path: ping() may attempt a (potentially blocking)
        // reconnect when the connection was lost between the state read and
        // the ping — enables readiness to recover connectivity on its own.
        probe.m_ping_ok = (m_ctx.m_nats_client->ping() == "PONG");
      }
    }
    // Default path: never initiate a reconnect from the health endpoint. Only
    // report the current connection state, so /health/ready answers fast for
    // the load balancer even while NATS is down.
    result = readiness::evaluate_nats(probe);
  } catch (const std::exception &e) {
    result = readiness::nats_check_failed(e.what());
  }

  // The reason of a refusal is visible only in the body and the log, so it is
  // counted as well: the metric answers "how often and why is the proxy not
  // ready" without scraping the health endpoint.
  if (!result.m_ready) {
    m_ctx.m_proxy.m_metrics->m_not_ready_total
        .Add({{"reason", result.m_reason}})
        .Increment();
  }

  write_nats_readiness(res, "l2-proxy",
                       m_ctx.m_proxy.m_metrics->m_nats_connected,
                       m_ctx.m_proxy.m_metrics->m_health_ready, result.m_ready,
                       result.m_error);
}

void RequestHandler::handle_duplicates(httplib::Response &res) const {
  // Simple report of duplicate POST requests detected from clients. When the
  // detector is absent (non-proxy mode) or disabled this endpoint answers 404
  // so callers can distinguish "not supported here" from "empty report".
  if (!m_ctx.m_proxy.m_duplicate_detector) {
    set_json_error_response(res, http_status::kNotFound,
                            "duplicate detection not available in this mode");
    return;
  }
  res.status = http_status::kOk;
  // Stored body samples may be binary (raw-body pass-through mode), which is
  // not valid UTF-8: replace undecodable bytes instead of throwing out of the
  // handler.
  res.set_content(m_ctx.m_proxy.m_duplicate_detector->report().dump(
                      2, ' ', false, nlohmann::json::error_handler_t::replace),
                  "application/json");
}

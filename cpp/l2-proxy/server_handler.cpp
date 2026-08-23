#include "server_handler.hpp"
#include "common_utils.hpp"
#include "httplib/httplib.h"
#include "logger.hpp"
#include "prometheus/counter.h"
#include "prometheus/registry.h"
#include "scoped_profiler.hpp"
#include "tracing_helpers.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <thread>
#include <unistd.h>

namespace {
// Minimal valid 1x1 32-bit ICO file (70 bytes): ICONDIR + ICONDIRENTRY +
// BITMAPINFOHEADER + XOR/AND masks. Served at /favicon.ico for the binary GET
// integrity test (starts with the 00 00 01 00 ICO header).
constexpr std::array<unsigned char, 70> g_favicon_ico = {
    0x00, 0x00, 0x01, 0x00, 0x01, 0x00, // ICONDIR
    0x01, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x20, 0x00, // ICONDIRENTRY (planes/bitcount)
    0x30, 0x00, 0x00, 0x00, 0x16, 0x00,
    0x00, 0x00, // size=48, offset=22
    0x28, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, // BITMAPINFOHEADER: size=40, width=1
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x20, 0x00, // height=2, planes=1, bpp=32
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, // compression=0, sizeimage=16
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, // xppm, yppm
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,             // clrused, clrimportant
    0xFF, 0xFF, 0xFF, 0xFF, // XOR mask (white, opaque)
    0x00, 0x00, 0x00, 0x00  // AND mask (padded to 4 bytes)
};

// Marker header sent by message_counter.py to opt in to the correlation-test
// echo (req_id + req_hash). Regular production clients do not send it, so the
// l2-server stays a plain value-echo backend without per-request SHA-256 cost.
constexpr char g_correlation_test_header[] = "X-Correlation-Test";

} // namespace

ServerHandler::ServerHandler(AppContext &ctx) : m_ctx(ctx) {}

void ServerHandler::send_response_with_trace(const httplib::Request &req,
                                             httplib::Response &res,
                                             json response_json,
                                             uint64_t start_us) {
  const TraceContext trace_ctx = TraceContextHelper::extract_and_validate(
      req.headers, m_ctx.m_tracer.get(), "ServerHandler");

  Logger::set_trace_id(trace_ctx.m_trace_id);

  response_json["server_span_id"] = trace_ctx.m_span_id;

  if (m_ctx.m_tracer) {
    Logger::debug(
        "ServerHandler: logging span to Jaeger - trace_id={} span_id={}",
        trace_ctx.m_trace_id, trace_ctx.m_span_id);
    const auto end_us = get_current_timestamp_us();
    m_ctx.m_tracer->log_request(req.method, "/", 200, start_us, end_us,
                                "l2-server", "", trace_ctx.m_trace_id,
                                trace_ctx.m_span_id, trace_ctx.m_parent_id);
  } else {
    Logger::warn("ServerHandler: tracer is null, Jaeger tracing disabled");
  }

  const std::string response_str = response_json.dump();
  Logger::debug("Server response body ({} bytes): {}", response_str.size(),
                log_body_preview(response_str));

  increment_and_log_response_sent(m_ctx.m_server.m_metrics->m_bytes_sent,
                                  "Server", "", 200);

  send_json_response(res, 200, response_json);
  set_traceparent_response_header(res, trace_ctx);
}

void ServerHandler::handle_post(const httplib::Request &req,
                                httplib::Response &res) {
  Logger::debug("ServerHandler::handle_post called - request received");
  Logger::debug("Server received POST path={} query_params_count={}", req.path,
                req.params.size());

  // Correlate log lines for this request via the thread-local context
  LogContextScope log_scope;
  std::string client_ip = extract_client_ip(req);
  if (client_ip.empty()) {
    client_ip = "unknown";
  }
  Logger::set_client_ip(client_ip);

  const RequestScopedTiming request_timing(
      m_ctx.m_server.m_metrics->m_request_duration_seconds,
      m_ctx.m_server.m_metrics->m_requests);

  const std::string &body = req.body;
  increment_and_log_request_received(m_ctx.m_server.m_metrics->m_bytes_received,
                                     "Server", body.size());

  const auto parse_result = validate_and_parse_json(body, "Server");
  if (!parse_result) {
    fail_request(res, 400, "Invalid JSON",
                 &m_ctx.m_server.m_metrics->m_request_errors, "",
                 parse_result.error());
    return;
  }

  const int value = parse_result->value("value", 0);

  // Test-mode delay: sleep a random amount before replying so response order
  // diverges from request arrival order. Used by the correlation test to
  // exercise response mixing under concurrency.
  apply_test_delay();

  const bool correlation_test =
      req.has_header(g_correlation_test_header) &&
      req.get_header_value(g_correlation_test_header) == "1";
  json response_json = {{"value_return", value}};
  if (correlation_test) {
    // Correlation-test echo: only message_counter.py sets the marker header,
    // so ordinary clients get a plain value echo with no SHA-256 overhead.
    response_json["req_id"] = parse_result->value("req_id", std::string{});
    // SHA-256 of the raw request body so the client can verify it received
    // exactly the response for its own (unique) body, not another request's.
    response_json["req_hash"] = compute_sha256_hex(body);
  }

  send_response_with_trace(req, res, response_json, request_timing.start_us());
}

void ServerHandler::handle_favicon(httplib::Response &res) const {
  res.set_content(
      std::string(reinterpret_cast<const char *>(g_favicon_ico.data()),
                  g_favicon_ico.size()),
      "image/x-icon");
}

void ServerHandler::apply_test_delay() const {
  const int max_delay_ms = m_ctx.m_config.m_test_response_delay_ms;
  if (max_delay_ms <= 0) {
    return;
  }
  // thread_local so each request handler thread gets its own generator
  thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> dist(0, max_delay_ms);
  const int delay_ms = dist(rng);
  Logger::debug("Test delay applied: {} ms (max={} ms)", delay_ms,
                max_delay_ms);
  std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}

void ServerHandler::handle_get(const httplib::Request &req,
                               httplib::Response &res) {
  Logger::debug("ServerHandler::handle_get called - request received");

  if (req.path == kHealthLivePath || req.path == kHealthPath) {
    set_health_alive(res, "l2-server");
    return;
  }

  if (req.path == kHealthReadyPath) {
    set_health_ready(res, "l2-server");
    m_ctx.m_server.m_metrics->m_health_ready.Set(1.0);
    return;
  }

  if (req.path == "/favicon.ico") {
    handle_favicon(res);
    return;
  }

  // Correlate log lines for this request via the thread-local context
  LogContextScope log_scope;
  std::string client_ip = extract_client_ip(req);
  if (client_ip.empty()) {
    client_ip = "unknown";
  }
  Logger::set_client_ip(client_ip);

  const RequestScopedTiming request_timing(
      m_ctx.m_server.m_metrics->m_request_duration_seconds,
      m_ctx.m_server.m_metrics->m_requests);

  const json response_json = {{"value_return", 0}};

  send_response_with_trace(req, res, response_json, request_timing.start_us());
}

#include "request_handler.hpp"
#include "app_context.hpp"
#include "common_utils.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "metrics_manager.hpp"
#include "request_data_preparer.hpp"
#include "response_builder.hpp"
#include "scoped_profiler.hpp"
#include "sentry_client.hpp"
#include "stats_logger.hpp"
#include "time_utils.hpp"
#include "trace_context_extractor.hpp"
#include "trace_logger.hpp"
#include "tracing_helpers.hpp"
#include "url_utils.hpp"
#include <format>
#include <string>
#include <nlohmann/json.hpp>
#include <prometheus/counter.h>

// Named struct for RAII active client tracking
struct ActiveClientTracker {
  StatsLogger &m_stats_logger;
  explicit ActiveClientTracker(StatsLogger &logger) : m_stats_logger(logger) {}
  ~ActiveClientTracker() { m_stats_logger.decrement_active_clients(); }
};

// ============================================================================
// Shared failure helpers
// ============================================================================
bool RequestHandler::fail_backend_request(
    httplib::Response &res, const std::string &method, const std::string &path,
    int status, const std::string &message, const std::string &category,
    const std::string &detail, uint64_t start_us, const TraceContext &trace_ctx,
    const std::string &request_id) const {
  BackendErrorSpanLogger::log_backend_error(
      m_ctx.m_tracer.get(), method, path, status, start_us, trace_ctx,
      m_ctx.m_config.m_app.m_mode, request_id, category, detail);
  if (m_ctx.m_sentry) {
    m_ctx.m_sentry->capture_message(
        std::format("Backend request failed: category={} message={} detail={} "
                    "path={}",
                    category, message, detail, path),
        request_id, {"proxy_backend_error", category});
  }
  return fail_request_categorized(res, status, message,
                                  &m_ctx.m_proxy.m_metrics->m_client_errors,
                                  request_id, detail);
}

// ============================================================================
// Phase 2: Extract trace context and create tracing spans
// ============================================================================
TraceContext RequestHandler::setup_tracing(
    const httplib::Request &req, const std::string &request_id,
    long long start_us, std::string &inlet_span_id,
    std::string &backend_push_span_id,
    std::string &traceparent_for_backend) const {
  const std::string traceparent_raw = get_traceparent_header(req.headers);
  const TraceContext trace_ctx = extract_trace_context(
      req, m_ctx.m_tracer.get(), backend_push_span_id, traceparent_for_backend);

  // A traceparent that is present but unusable (broken client, or noise that
  // looks like a trace): the request is still served, but the trace is not
  // continued -- worth counting separately from the requests without any
  // traceparent at all.
  if (!traceparent_raw.empty() && trace_ctx.m_trace_id.empty()) {
    m_ctx.m_tracing_metrics->m_traceparent_invalid.Increment();
  }

  inlet_span_id =
      log_incoming_span(m_ctx.m_tracer.get(), "/", start_us, request_id,
                        trace_ctx);

  return trace_ctx;
}

// ============================================================================
// Phase 3b: Push request to backend (NATS)
// ============================================================================
std::string RequestHandler::push_to_backend(
    nlohmann::json request_data, const std::string &request_id,
    const std::string &trace_id, const std::string &backend_push_span_id,
    const TraceContext &trace_ctx) const {
  Logger::debug("Proxy queueing request via NATS: request_id={} size={}",
                request_id, request_data.size());

  auto request_json = m_services.m_push_service.push_request(
      std::move(request_data), trace_id, backend_push_span_id, trace_ctx);
  if (request_json.empty()) {
    Logger::error("Failed to push request to backend: request_id={}",
                  request_id);
    return "";
  }

  Logger::info("Request queued via NATS: request_id={}", request_id);
  return request_json;
}

// ============================================================================
// Phase 3c: Poll for response from backend
// ============================================================================
std::string RequestHandler::poll_for_response(const std::string &request_id,
                                              const std::string &request_json,
                                              const TraceContext &trace_ctx) {
  return m_services.m_poll_service.poll_response(request_id, request_json,
                                      m_config.m_request_timeout_seconds, trace_ctx);
}

// ============================================================================
// Phase 4: Send response to client
// ============================================================================
void RequestHandler::send_response(
    httplib::Response &res, const nlohmann::json &response_data,
    const std::string &request_id, const TraceContext &trace_ctx,
    const std::string &method, const std::string &path, long long start_us) {
  set_response_content(res, response_data, request_id, trace_ctx, method, path,
                       start_us, m_ctx);

  size_t response_size = res.body.size();
  const long long end_us = get_current_timestamp_us();
  Logger::info("Request completed: request_id={} path={} status={} "
               "response_size={} bytes duration_ms={}",
               request_id, path, res.status, response_size,
               (end_us - start_us) / TimeUtils::kUsPerMs);
}

// ============================================================================
// Phase 3: Process request, push to backend, poll for response, cache result
// ============================================================================
void RequestHandler::process_request(const std::string &method,
                                     const std::string &path,
                                     const std::string &body,
                                     const httplib::Request &req,
                                     httplib::Response &res,
                                     const std::string &client_ip) {
  // Track in-flight request for graceful shutdown
  const auto in_flight_guard = m_ctx.m_in_flight_tracker.track();

  // Track active client
  m_stats_logger.increment_active_clients();

  const RequestScopedTiming request_timing(
      m_ctx.m_proxy.m_metrics->m_request_duration_seconds,
      m_ctx.m_proxy.m_metrics->m_client_requests);

  // RAII: decrement active clients on exit
  ActiveClientTracker const tracker(m_stats_logger);

  Logger::debug("Proxy received request.path: {}", req.path);
  Logger::debug("Proxy received request body ({} bytes): {}", req.body.size(),
                log_body_preview(req.body));

  const auto request_id = m_services.m_id_generator.generate_uuid();
  Logger::set_request_id(request_id);

  // Extract IPs for logging (client_ip comes from the ScopedRequestContext so
  // the header is parsed once per request, see handle_request).
  const auto proxy_ip = extract_proxy_ip(req);

  Logger::info("Proxy received {} request client_ip={} proxy_ip={} path={} "
               "request_id={}",
               method, client_ip, proxy_ip, path, request_id);
  Logger::info("Proxy request msg_size={}", body.size());

  // Setup tracing
  std::string inlet_span_id, backend_push_span_id, traceparent_for_backend;
  const TraceContext trace_ctx =
      setup_tracing(req, request_id, request_timing.start_us(), inlet_span_id,
                    backend_push_span_id, traceparent_for_backend);
  Logger::set_trace_id(trace_ctx.m_trace_id);
  const auto trace_id =
      resolve_trace_id(m_ctx.m_tracer.get(), trace_ctx);

  // Prepare request data for backend
  nlohmann::json request_data = prepare_request_data(
      request_id, method, path, body, req, traceparent_for_backend);

  // Add proxy span info for tracing chain
  if (m_ctx.m_tracer) {
    const auto nats_push_span_id = JaegerLogger::generate_span_id();
    add_proxy_trace_fields(request_data, m_ctx.m_tracer.get(), trace_ctx,
                           inlet_span_id, nats_push_span_id);
  }

  // Push to backend
  auto request_json =
      push_to_backend(std::move(request_data), request_id, trace_id,
                      backend_push_span_id, trace_ctx);
  if (request_json.empty()) {
    fail_backend_request(res, method, path, http_status::kInternalServerError,
                         "Failed to queue request", "queue_failed",
                         "Failed to queue request via NATS",
                         request_timing.start_us(), trace_ctx, request_id);
    return;
  }

  // Poll for response
  const std::string response_data_str =
      poll_for_response(request_id, request_json, trace_ctx);

  if (response_data_str.empty()) {
    fail_backend_request(
        res, method, path, http_status::kGatewayTimeout,
        "Timeout waiting for response", "empty_response",
        std::format("Empty response received from NATS for request_id={}",
                    request_id),
        request_timing.start_us(), trace_ctx, request_id);
    return;
  }

  // Parse response for sending
  const auto parse_result = JsonUtils::try_parse(response_data_str);

  // Send response to client
  if (parse_result) {
    send_response(res, *parse_result, request_id, trace_ctx, method, path,
                  request_timing.start_us());
  } else {
    fail_backend_request(
        res, method, path, http_status::kInternalServerError,
        "Invalid response format", "invalid_response",
        std::format("Failed to parse response for request_id={}: {}",
                    request_id, parse_result.error()),
        request_timing.start_us(), trace_ctx, request_id);
    return;
  }
}

// ============================================================================
// Main request handler — orchestrates all phases
// ============================================================================
void RequestHandler::handle_request(const httplib::Request &req,
                                    httplib::Response &res,
                                    const std::string &method,
                                    const std::string &body) {
  // Extract client IP (X-Real-IP/X-Forwarded-For from the trusted nginx).
  // Used consistently for per-IP rate limiting and the correlation context; the
  // value lives in the ScopedRequestContext created below.
  // Correlate every log line of this request via the thread-local context.
  // request_id/trace_id are set later in process_request once they exist;
  // the scope restores the previous values on exit (covers keep-alive reuse).
  ScopedRequestContext const req_ctx(req);
  const auto &client_ip = req_ctx.client_ip();

  // Per-client distribution metric: X-DataHub-Client-Id header tells apart
  // clients that share one IP (e.g. behind NAT) in Grafana.
  const auto client_id =
      get_header_value(req.headers, "X-DataHub-Client-Id", "unknown");
  increment_per_client_metric(m_ctx.m_proxy.m_per_client_id_metrics_collector,
                              client_id, 0);

  // Duplicate POST detection: the same request body (SHA-256 hash) delivered
  // more than once counts as a duplicate from a client. GET favicon probes are
  // ignored; the hashing cost is skipped entirely when the feature is off.
  if (method == "POST" && !body.empty() &&
      m_ctx.m_config.m_duplicate.m_enabled &&
      m_ctx.m_proxy.m_duplicate_detector &&
      record_and_maybe_reject_duplicate(client_id, client_ip, body, res)) {
    return;
  }

  // Per-client latency: RAII observer into the labeled histogram, covers the
  // whole handling (rate-limit checks + backend round-trip) for every exit
  // path, like the global ScopedProfiler in process_request.
  const ScopedLabeledProfiler client_latency_profiler(
      m_ctx.m_proxy.m_per_client_id_latency_collector.get(), client_id);

  // Phase 1: Rate limiting
  if (!check_rate_limits(client_ip, client_id, req, res)) {
    return;
  }

  const auto &path = req.path;

  // Phase 2: Process request (push to backend, poll, cache)
  process_request(method, path, body, req, res, client_ip);
}

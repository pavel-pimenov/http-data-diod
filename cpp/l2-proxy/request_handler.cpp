#include "request_handler.hpp"
#include "common_utils.hpp"
#include "db_query_utils.hpp"
#include "stats_page.hpp"
#include "duplicate_detector.hpp"
#include "exceptions.hpp"
#include "http_client.hpp"
#include "httplib/httplib.h"
#include "json_utils.hpp"
#include "labeled_counter_collector.hpp"
#include "logger.hpp"
#include "metrics_manager.hpp"
#include "nats_client.hpp"
#include "rate_limiter.hpp"
#include "rate_limiter_per_ip.hpp"
#include "retry_utils.hpp"
#include "scoped_metrics.hpp"
#include "scoped_profiler.hpp"
#include "trace_logger.hpp"
#include "tracing_helpers.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <nlohmann/json.hpp>
#include <prometheus/counter.h>
#include <prometheus/registry.h>
#include <random>
#include <sstream>
#include <thread>

// Named struct for RAII active client tracking
struct ActiveClientTracker {
  StatsLogger *m_stats_logger;
  explicit ActiveClientTracker(StatsLogger *logger) : m_stats_logger(logger) {}
  ~ActiveClientTracker() {
    if (m_stats_logger) {
      m_stats_logger->decrement_active_clients();
    }
  }
};

// URL prefix of the HTTP DB Gateway endpoints (docs/openapi/http-db-gate.yaml).
inline constexpr const char *kDbGatewayPath = "/v1/sql";

// Maps an unknown-db error into a ready-to-send HTTP response.
void send_db_error(httplib::Response &res, int status, const std::string &code,
                   const std::string &message) {
  res.status = status;
  res.set_content(make_db_error_body(status, code, message).dump(),
                  "application/json");
}

RequestHandler::RequestHandler(AppContext &ctx, StatsLogger *stats_logger)
    : m_ctx(ctx), m_stats_logger(stats_logger),
      m_request_timeout_seconds(g_default_request_timeout_seconds),
      m_id_generator(), m_push_service(ctx), m_poll_service(ctx) {
  m_request_timeout_seconds = m_ctx.m_config.m_request_timeout_seconds;
}

RequestHandler::~RequestHandler() {}

void RequestHandler::handle_get(const httplib::Request &req,
                                httplib::Response &res) {
  // Handle health check endpoints
  if (req.path == kHealthLivePath || req.path == kHealthPath) {
    // Liveness probe - just check if process is running
    set_health_alive(res, "l2-proxy");
    return;
  }

  // Lightweight HTML status page (no Grafana needed) sourced from the Prometheus
  // registry. Lets operators assess service health directly.
  if (req.path == "/stats") {
    res.set_content(build_stats_html("l2-proxy", m_ctx.m_proxy_registry),
                    "text/html; charset=utf-8");
    return;
  }

  if (req.path == "/crash-test") {
    // Intentionally crashes the process to test the crash handler. Guarded
    // behind ENABLE_CRASH_TEST_ENDPOINT (default off) so a public client can
    // not remotely SIGSEGV the proxy.
    if (!m_ctx.m_config.m_enable_crash_test_endpoint) {
      res.status = 404;
      res.set_content(
          R"json({"error": "crash test endpoint is disabled (ENABLE_CRASH_TEST_ENDPOINT=false)"})json",
          "application/json");
      return;
    }
    Logger::error("CRASH TEST: intentional SIGSEGV via /crash-test endpoint");
    volatile int *bad_ptr = nullptr;
    // cppcheck-suppress nullPointer
    *bad_ptr = 42; // NOLINT //-V522 triggers SIGSEGV for crash handler test
    // unreachable
    res.status = 200;
    return;
  }

  if (req.path == kHealthReadyPath) {
    bool nats_healthy = false;
    std::string error_msg;

    try {
      if (m_ctx.m_nats_client) {
        if (m_ctx.m_config.m_health_ready_allow_connect) {
          // Opt-in legacy path: ping() may attempt a (potentially blocking)
          // reconnect when the connection was lost between the state read and
          // the ping — enables readiness to recover connectivity on its own.
          if (m_ctx.m_nats_client->is_connected()) {
            nats_healthy = (m_ctx.m_nats_client->ping() == "PONG");
            if (!nats_healthy) {
              error_msg = "NATS ping failed";
            }
          } else {
            error_msg = "NATS connection not available";
          }
        } else {
          // Default: never initiate a reconnect from the health endpoint.
          // Only report the current connection state, so /health/ready answers
          // fast for the load balancer even while NATS is down.
          nats_healthy = m_ctx.m_nats_client->is_connected();
          if (!nats_healthy) {
            error_msg = "NATS connection not available";
          }
        }
      } else {
        error_msg = "NATS connection not available";
      }
    } catch (const std::exception &e) {
      error_msg = std::format("NATS health check failed: {}", e.what());
      nats_healthy = false;
    }

    m_ctx.m_proxy.m_metrics->m_nats_connected.Set(nats_healthy ? 1.0 : 0.0);
    m_ctx.m_proxy.m_metrics->m_health_ready.Set(nats_healthy ? 1.0 : 0.0);

    if (nats_healthy) {
      res.status = 200;
      res.set_content(
          R"({"status": "ready", "service": "l2-proxy", "messaging": "nats"})",
          "application/json");
    } else {
      res.status = 503;
      nlohmann::json error_body;
      error_body["status"] = "not_ready";
      error_body["service"] = "l2-proxy";
      error_body["error"] = error_msg;
      send_json_response(res, res.status, error_body);
    }
    return;
  }

  if (req.path == "/debug/duplicates") {
    // Simple report of duplicate POST requests detected from clients. When the
    // detector is absent (non-proxy mode) or disabled, still answers 200 with
    // the enabled flag so callers can distinguish "off" from "empty".
    if (!m_ctx.m_proxy.m_duplicate_detector) {
      res.status = 404;
      res.set_content(
          R"({"error": "duplicate detection not available in this mode"})",
          "application/json");
      return;
    }
    res.status = 200;
    res.set_content(m_ctx.m_proxy.m_duplicate_detector->report().dump(2),
                    "application/json");
    return;
  }

  if (req.path.starts_with(kDbGatewayPath)) {
    handle_db_gateway(req, res, "GET", "");
    return;
  }

  handle_request(req, res, "GET", "");
}

void RequestHandler::handle_post(const httplib::Request &req,
                                 httplib::Response &res) {
  const std::string &body = req.body;
  increment_and_log_request_received(m_ctx.m_proxy.m_metrics->m_bytes_received,
                                     "Proxy", body.size());

  // Record request size in histogram
  m_ctx.m_proxy.m_metrics->m_request_size_bytes.Observe(
      static_cast<double>(body.size()));

  // Validate that the body is valid JSON (lightweight syntax check without DOM
  // allocation)
  if (!body.empty() && !nlohmann::json::accept(body)) {
    fail_request(res, 400, "Invalid JSON in request body",
                 &m_ctx.m_proxy.m_metrics->m_client_errors);
    return;
  }

  if (req.path.starts_with(kDbGatewayPath)) {
    handle_db_gateway(req, res, "POST", body);
    return;
  }

  handle_request(req, res, "POST", body);
}

// ============================================================================
// Phase 1: Rate limiting (global + per-IP)
// ============================================================================
bool RequestHandler::check_rate_limits(const std::string &client_ip,
                                       const std::string &client_id,
                                       const httplib::Request &req,
                                       httplib::Response &res) {
  // Trace context is extracted here (before setup_tracing) so that 429
  // rejections get their own span in Jaeger.
  const std::string traceparent_raw = get_traceparent_header(req.headers);

  // Check global rate limiter first
  if (m_ctx.m_proxy.m_rate_limiter &&
      !m_ctx.m_proxy.m_rate_limiter->acquire()) {
    Logger::debug(
        "Global rate limit exceeded: token bucket empty (max={} burst, "
        "refill={}/s). Sustained request rate above global capacity — "
        "consider raising GLOBAL_RATE_LIMIT_MAX_TOKENS / "
        "GLOBAL_RATE_LIMIT_REFILL_RATE env vars",
        m_ctx.m_proxy.m_rate_limiter->max_tokens(),
        m_ctx.m_proxy.m_rate_limiter->refill_rate());
    return reject_rate_limited(
        res, client_ip, client_id, traceparent_raw, "global",
        m_ctx.m_proxy.m_rate_limiter_metrics->m_rejected_requests,
        "Global rate limit exceeded. Please retry later.",
        m_ctx.m_proxy.m_rate_limiter->max_tokens(),
        m_ctx.m_proxy.m_rate_limiter->available_tokens());
  }

  // Check per-IP rate limiter if enabled
  if (m_ctx.m_proxy.m_per_ip_rate_limiter &&
      !m_ctx.m_proxy.m_per_ip_rate_limiter->acquire(client_ip)) {
    return reject_rate_limited(
        res, client_ip, client_id, traceparent_raw, "per_ip",
        m_ctx.m_proxy.m_per_ip_rate_limiter_metrics->m_rejected_requests,
        "Rate limit exceeded for your IP: " + client_ip +
            ". Please retry later.",
        m_ctx.m_proxy.m_per_ip_rate_limiter->max_tokens_per_ip(), 0);
  }
  if (m_ctx.m_proxy.m_per_ip_rate_limiter) {
    m_ctx.m_proxy.m_internal_memory_metrics->m_per_ip_rate_limiter_ips_tracked
        .Set(static_cast<double>(
            m_ctx.m_proxy.m_per_ip_rate_limiter->get_stats().m_tracked_ips));
  }

  // Update rate limiter metrics
  if (m_ctx.m_proxy.m_rate_limiter) {
    m_ctx.m_proxy.m_rate_limiter_metrics->m_available_tokens.Set(
        m_ctx.m_proxy.m_rate_limiter->available_tokens());
  }

  return true;
}

// ============================================================================
// Shared failure helpers
// ============================================================================
bool RequestHandler::fail_backend_request(
    httplib::Response &res, const std::string &method, const std::string &path,
    int status, const std::string &message, const std::string &category,
    const std::string &detail, uint64_t start_us, const TraceContext &trace_ctx,
    const std::string &request_id) {
  BackendErrorSpanLogger::log_backend_error(
      m_ctx.m_tracer.get(), method, path, status, start_us, trace_ctx,
      m_ctx.m_config.m_mode, request_id, category, detail);
  return fail_request(res, status, message,
                      &m_ctx.m_proxy.m_metrics->m_client_errors, request_id,
                      detail);
}

bool RequestHandler::reject_rate_limited(
    httplib::Response &res, const std::string &client_ip,
    const std::string &client_id, const std::string &traceparent_raw,
    const std::string &reason, prometheus::Counter &rejected_counter,
    const std::string &message, uint64_t limit, uint64_t remaining) {
  rejected_counter.Increment();
  if (m_ctx.m_proxy.m_per_client_id_metrics_collector) {
    m_ctx.m_proxy.m_per_client_id_metrics_collector->record_rejection(
        client_id);
  }
  res.set_header("Retry-After", "1");
  res.set_header("X-RateLimit-Limit", std::to_string(limit));
  res.set_header("X-RateLimit-Remaining", std::to_string(remaining));
  RateLimitSpanLogger::log_rate_limit_rejection(
      m_ctx.m_tracer.get(), reason, client_ip, traceparent_raw,
      std::to_string(limit), std::to_string(remaining));
  return fail_request(res, 429, message,
                      &m_ctx.m_proxy.m_metrics->m_client_errors);
}

// ============================================================================
// Phase 2: Extract trace context and create tracing spans
// ============================================================================
TraceContext RequestHandler::setup_tracing(
    const httplib::Request &req, const std::string &request_id,
    long long start_us, std::string &inlet_span_id,
    std::string &backend_push_span_id, std::string &traceparent_for_backend) {
  const TraceContext trace_ctx = extract_trace_context(
      req, m_ctx.m_tracer.get(), backend_push_span_id, traceparent_for_backend);

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
    const TraceContext &trace_ctx) {
  Logger::debug("Proxy queueing request via NATS: request_id={} size={}",
                request_id, request_data.size());

  std::string request_json = m_push_service.push_request(
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
  std::string response_data_str;
  try {
    response_data_str = m_poll_service.poll_response(
        request_id, request_json, m_request_timeout_seconds, trace_ctx);
  } catch (const TimeoutException &e) {
    Logger::error("Timeout waiting for response: request_id={} error={}",
                  request_id, e.what());
    throw; // Re-throw for caller to handle
  }
  return response_data_str;
}

// ============================================================================
// Phase 4: Send response to client
// ============================================================================
void RequestHandler::send_response(
    httplib::Response &res, const nlohmann::json &response_data,
    const std::string &request_id, const TraceContext &trace_ctx,
    const std::string &method, const std::string &path, long long start_us) {
  set_response_content(res, response_data, request_id, trace_ctx, method, path,
                       start_us, m_ctx, m_stats_logger);

  size_t response_size = res.body.size();
  const long long end_us = get_current_timestamp_us();
  Logger::info("Request completed: request_id={} path={} status={} "
               "response_size={} bytes duration_ms={}",
               request_id, path, res.status, response_size,
               (end_us - start_us) / 1000);
}

// ============================================================================
// Phase 3: Process request, push to backend, poll for response, cache result
// ============================================================================
bool RequestHandler::process_request(const std::string &method,
                                     const std::string &path,
                                     const std::string &body,
                                     const httplib::Request &req,
                                     httplib::Response &res,
                                     const std::string &client_ip) {
  // Track in-flight request for graceful shutdown
  const auto in_flight_guard = m_ctx.m_in_flight_tracker.track();

  // Track active client if stats logger is available
  if (m_stats_logger) {
    m_stats_logger->increment_active_clients();
    m_stats_logger->increment_total_requests();
  }

  const RequestScopedTiming request_timing(
      m_ctx.m_proxy.m_metrics->m_request_duration_seconds,
      m_ctx.m_proxy.m_metrics->m_client_requests);

  // RAII: decrement active clients on exit
  ActiveClientTracker tracker(m_stats_logger);

  Logger::debug("Proxy received request.path: {}", req.path);
  Logger::debug("Proxy received request body ({} bytes): {}", req.body.size(),
                log_body_preview(req.body));

  const std::string request_id = m_id_generator.generate_uuid();
  Logger::set_request_id(request_id);

  // Extract IPs for logging
  std::string effective_client_ip = extract_client_ip(req);
  const std::string proxy_ip = extract_proxy_ip(req);

  Logger::info("Proxy received {} request client_ip={} proxy_ip={} path={} "
               "request_id={}",
               method, effective_client_ip, proxy_ip, path, request_id);
  Logger::info("Proxy request msg_size={}", body.size());

  // Setup tracing
  std::string inlet_span_id, backend_push_span_id, traceparent_for_backend;
  const TraceContext trace_ctx =
      setup_tracing(req, request_id, request_timing.start_us(), inlet_span_id,
                    backend_push_span_id, traceparent_for_backend);
  Logger::set_trace_id(trace_ctx.m_trace_id);
  const std::string trace_id =
      resolve_trace_id(m_ctx.m_tracer.get(), trace_ctx);

  // Prepare request data for backend
  nlohmann::json request_data = prepare_request_data(
      request_id, method, path, body, req, traceparent_for_backend);

  // Add proxy span info for tracing chain
  if (m_ctx.m_tracer) {
    const std::string nats_push_span_id = m_ctx.m_tracer->generate_span_id();
    add_proxy_trace_fields(request_data, m_ctx.m_tracer.get(), trace_ctx,
                           inlet_span_id, nats_push_span_id);
  }

  // Push to backend
  std::string request_json =
      push_to_backend(std::move(request_data), request_id, trace_id,
                      backend_push_span_id, trace_ctx);
  if (request_json.empty()) {
    return fail_backend_request(
        res, method, path, 500, "Failed to queue request", "queue_failed",
        "Failed to queue request via NATS", request_timing.start_us(),
        trace_ctx, request_id);
  }

  // Poll for response
  std::string response_data_str;
  try {
    response_data_str = poll_for_response(request_id, request_json, trace_ctx);
  } catch (const TimeoutException &e) {
    return fail_backend_request(
        res, method, path, 504, "Timeout waiting for response", "timeout",
        std::format("Timeout: {}", e.what()), request_timing.start_us(),
        trace_ctx, request_id);
  }

  if (response_data_str.empty()) {
    return fail_backend_request(
        res, method, path, 504, "Timeout waiting for response",
        "empty_response",
        std::format("Empty response received from NATS for request_id={}",
                    request_id),
        request_timing.start_us(), trace_ctx, request_id);
  }

  // Parse response for sending
  const auto parse_result = JsonUtils::try_parse(response_data_str);

  // Send response to client
  if (parse_result) {
    send_response(res, *parse_result, request_id, trace_ctx, method, path,
                  request_timing.start_us());
  } else {
    return fail_backend_request(
        res, method, path, 500, "Invalid response format", "invalid_response",
        std::format("Failed to parse response for request_id={}: {}",
                    request_id, parse_result.error()),
        request_timing.start_us(), trace_ctx, request_id);
  }

  return true;
}

// ============================================================================
// Main request handler — orchestrates all phases
// ============================================================================
void RequestHandler::handle_request(const httplib::Request &req,
                                    httplib::Response &res,
                                    const std::string &method,
                                    const std::string &body) {
  // Extract client IP (X-Real-IP/X-Forwarded-For from the trusted nginx).
  // Used consistently for per-IP rate limiting and the correlation context.
  std::string client_ip = extract_client_ip(req);
  if (client_ip.empty()) {
    client_ip = "unknown";
  }

  // Per-client distribution metric: X-DataHub-Client-Id header tells apart
  // clients that share one IP (e.g. behind NAT) in Grafana.
  const std::string client_id =
      get_header_value(req.headers, "X-DataHub-Client-Id", "unknown");
  if (m_ctx.m_proxy.m_per_client_id_metrics_collector) {
    m_ctx.m_proxy.m_per_client_id_metrics_collector->record_request(client_id);
  }

  // Duplicate POST detection: the same request body (SHA-256 hash) delivered
  // more than once counts as a duplicate from a client. GET favicon probes are
  // ignored; the hashing cost is skipped entirely when the feature is off.
  if (method == "POST" && !body.empty() &&
      m_ctx.m_config.m_duplicate_detection_enabled &&
      m_ctx.m_proxy.m_duplicate_detector) {
    const std::string body_hash = compute_sha256_hex(body);
    if (m_ctx.m_proxy.m_duplicate_detector->record(client_id, body_hash,
                                                   body)) {
      m_ctx.m_proxy.m_metrics->m_duplicate_posts_detected.Increment();
      if (m_ctx.m_proxy.m_per_client_id_duplicate_collector) {
        m_ctx.m_proxy.m_per_client_id_duplicate_collector->record_request(
            client_id);
      }
      Logger::warn("Duplicate POST detected: client_id={} body_bytes={}",
                   client_id, body.size());

      // When enabled, reject the duplicate instead of forwarding it to the
      // worker: the body was already delivered within the TTL window, so
      // forwarding would only repeat a side effect. Off by default — the
      // proxy then still counts and reports the duplicate (see
      // /debug/duplicates and l2_proxy_per_client_id_duplicate_* metrics).
      if (m_ctx.m_config.m_duplicate_reject_enabled) {
        if (m_ctx.m_proxy.m_per_client_id_duplicate_collector) {
          m_ctx.m_proxy.m_per_client_id_duplicate_collector->record_rejection(
              client_id);
        }
        res.status = 409;
        send_json_response(res, res.status,
                          make_error_json("duplicate request"));
        return;
      }
    }
  }

  // Per-client latency: RAII observer into the labeled histogram, covers the
  // whole handling (rate-limit checks + backend round-trip) for every exit
  // path, like the global ScopedProfiler in process_request.
  const ScopedLabeledProfiler client_latency_profiler(
      m_ctx.m_proxy.m_per_client_id_latency_collector.get(), client_id);

  // Correlate every log line of this request via the thread-local context.
  // request_id/trace_id are set later in process_request once they exist;
  // the scope restores the previous values on exit (covers keep-alive reuse).
  LogContextScope log_scope;
  Logger::set_client_ip(client_ip);

  // Phase 1: Rate limiting
  if (!check_rate_limits(client_ip, client_id, req, res)) {
    return;
  }

  const std::string &path = req.path;

  // Phase 2: Process request (push to backend, poll, cache)
  process_request(method, path, body, req, res, client_ip);
}

void RequestHandler::handle_db_gateway(const httplib::Request &req,
                                       httplib::Response &res,
                                       const std::string &method,
                                       const std::string &body) {
  // Distributed tracing by analogy with the main request path: extract (or
  // generate) the trace context, log the INCOMING span and correlate all DB
  // gateway log lines via the thread-local request context.
  const uint64_t start_us = get_current_timestamp_us();
  const std::string traceparent_raw = get_traceparent_header(req.headers);
  const TraceContext trace_ctx =
      handle_trace_context(traceparent_raw, m_ctx.m_tracer.get());
  const std::string request_id = m_id_generator.generate_uuid();

  LogContextScope log_scope;
  Logger::set_request_id(request_id);
  Logger::set_trace_id(trace_ctx.m_trace_id);
  Logger::set_client_ip(extract_client_ip(req));

  std::string inlet_span_id;
  inlet_span_id =
      log_incoming_span(m_ctx.m_tracer.get(), req.path, start_us, request_id,
                        trace_ctx);

  // Attach the proxy's tracing context to the DB request so the worker can
  // extend the same trace (analogous to NatsPushService on the main path).
  auto add_trace_fields = [&](json &request_json) {
    if (!m_ctx.m_tracer) {
      return;
    }
    const std::string nats_db_span_id = m_ctx.m_tracer->generate_span_id();
    add_proxy_trace_fields(request_json, m_ctx.m_tracer.get(), trace_ctx,
                           inlet_span_id, nats_db_span_id,
                           trace_ctx.m_sampled);
  };

  if (!m_ctx.m_config.m_db_query_enabled) {
    send_db_gateway_error(res, 404, "NOT_FOUND", "DB gateway is disabled",
                          method, req.path, start_us, trace_ctx, request_id);
    return;
  }

  std::string path_rest = req.path.substr(std::string(kDbGatewayPath).size());
  while (!path_rest.empty() && path_rest.front() == '/') {
    path_rest.erase(0, 1);
  }
  while (!path_rest.empty() && path_rest.back() == '/') {
    path_rest.pop_back();
  }

  if (path_rest.empty()) {
    if (method != "GET") {
      send_db_gateway_error(res, 405, "METHOD_NOT_ALLOWED", "Use GET", method,
                            req.path, start_us, trace_ctx, request_id);
      return;
    }
    json names = json::array();
    for (const DbConfig &db : m_ctx.m_config.m_databases) {
      names.push_back(json{{"name", db.m_name},
                           {"driver", db.m_driver},
                           {"enabled", true}});
    }
    res.status = 200;
    send_json_response(res, res.status, json{{"databases", names}});
    return;
  }

  const size_t slash = path_rest.find('/');
  const std::string db_name = path_rest.substr(0, slash);
  const std::string action =
      slash == std::string::npos ? "" : path_rest.substr(slash + 1);
  if (db_name.empty() || action.empty() ||
      action.find('/') != std::string::npos) {
    send_db_gateway_error(res, 404, "NOT_FOUND", "Unknown DB gateway path",
                          method, req.path, start_us, trace_ctx, request_id);
    return;
  }

  const bool known_db = std::ranges::any_of(
      m_ctx.m_config.m_databases,
      [&db_name](const DbConfig &db) { return db.m_name == db_name; });
  if (!known_db) {
    send_db_gateway_error(res, 404, "UNKNOWN_DATABASE",
                          std::format("Unknown database '{}'", db_name),
                          method, req.path, start_us, trace_ctx, request_id);
    return;
  }

  if (action == "query" && method == "POST") {
    const auto parsed_body = JsonUtils::try_parse(body);
    if (!parsed_body) {
      send_db_gateway_error(res, 400, "BAD_REQUEST", "Invalid JSON body",
                            method, req.path, start_us, trace_ctx, request_id);
      return;
    }
    json request = build_db_query_request(DbQueryContract::kTypeQuery,
                                          request_id, db_name, *parsed_body);
    add_trace_fields(request);
    const auto validated = parse_db_query_request(request);
    if (!validated) {
      send_db_gateway_error(res, 400, "BAD_REQUEST", validated.error(), method,
                            req.path, start_us, trace_ctx, request_id);
      return;
    }
    route_db_request(res, request, method, req.path, start_us, trace_ctx,
                     request_id, inlet_span_id);
    return;
  }

  if (action == "ping" && method == "GET") {
    json request = build_db_query_request(DbQueryContract::kTypePing,
                                          request_id, db_name);
    add_trace_fields(request);
    route_db_request(res, request, method, req.path, start_us, trace_ctx,
                     request_id, inlet_span_id);
    return;
  }

  send_db_gateway_error(res, 404, "NOT_FOUND",
                        std::format("Unsupported DB gateway action '{}' for {}",
                                    action, method),
                        method, req.path, start_us, trace_ctx, request_id);
}

void RequestHandler::send_db_gateway_error(
    httplib::Response &res, int status, const std::string &code,
    const std::string &message, const std::string &method,
    const std::string &path, uint64_t start_us, const TraceContext &trace_ctx,
    const std::string &request_id) {
  if (m_ctx.m_tracer) {
    JaegerSpanLogger::log_proxy_response(
        m_ctx.m_tracer.get(), method, path, status, start_us,
        get_current_timestamp_us(), trace_ctx, m_ctx.m_config.m_mode,
        request_id);
  }
  send_db_error(res, status, code, message);
}

void RequestHandler::route_db_request(
    httplib::Response &res, const json &request, const std::string &method,
    const std::string &path, uint64_t start_us, const TraceContext &trace_ctx,
    const std::string &request_id, const std::string &inlet_span_id) {
  const std::string db_name =
      JsonUtils::safe_get_string(request, DbQueryContract::kDb);

  // Records the gateway counters/histograms on every exit path (success and
  // each failure branch) so a stuck/erroring DB gateway is visible in
  // Prometheus, not just in traces.
  auto record_db_metrics = [this, &request, &db_name, start_us](int status) {
    const std::string type =
        JsonUtils::safe_get_string(request, DbQueryContract::kType);
    record_db_request_metrics(m_ctx.m_proxy.m_metrics->m_db_requests_total,
                             db_name, type, status);
    observe_db_request_duration(
        m_ctx.m_proxy.m_metrics->m_db_request_duration_seconds, db_name,
        start_us, get_current_timestamp_us());
  };

  if (!m_ctx.m_nats_client) {
    send_db_gateway_error(res, 503, "DB_UNAVAILABLE",
                          "NATS client is not available", method, path,
                          start_us, trace_ctx, request_id);
    record_db_metrics(503);
    return;
  }

  const std::string request_json = request.dump();
  const int64_t nats_start_us = TimeUtils::epoch_us();
  const std::string trace_id = resolve_trace_id(m_ctx.m_tracer.get(), trace_ctx);
  std::string nats_db_span_id =
      JsonUtils::safe_get_string(request, NatsContract::kProxySpanId);
  std::string nats_parent_id =
      resolve_parent_id(inlet_span_id, trace_ctx.m_parent_id);

  // request_with_consume_span_id also returns the worker's consume span id
  // (NATS header), which links the round-trip span to the worker's consume
  // span.
  auto [reply, consume_span_id] =
      m_ctx.m_nats_client->request_with_consume_span_id(
          m_ctx.m_config.m_db_query_nats_subject, request_json,
          m_ctx.m_config.m_db_query_nats_timeout_ms);
  const int64_t nats_end_us = TimeUtils::epoch_us();

  if (reply.m_data.empty()) {
    const std::string last_error =
        m_ctx.m_nats_client->get_last_error().value_or("");
    if (m_ctx.m_tracer && !trace_id.empty()) {
      nlohmann::json attrs = {
          {"nats.success", false},
          {"nats.destination", m_ctx.m_config.m_db_query_nats_subject},
          {"nats.duration_us", nats_end_us - nats_start_us},
          {"db.name", db_name},
      };
      if (!last_error.empty()) {
        attrs["nats.last_error"] = last_error;
      }
      JaegerSpanLogger::log_nats_span(m_ctx.m_tracer.get(), "NATS_db_request",
                                      500, request_id, trace_id,
                                      nats_db_span_id, nats_parent_id,
                                      nats_start_us, attrs);
    }
    if (!m_ctx.m_nats_client->is_connected()) {
      send_db_gateway_error(res, 503, "DB_UNAVAILABLE",
                            "NATS connection is not available", method, path,
                            start_us, trace_ctx, request_id);
      record_db_metrics(503);
    } else {
      send_db_gateway_error(res, 504, "TIMEOUT",
                            "DB worker did not respond in time", method, path,
                            start_us, trace_ctx, request_id);
      record_db_metrics(504);
    }
    observe_db_request_duration(
        m_ctx.m_proxy.m_metrics->m_db_nats_request_duration_seconds, db_name,
        nats_start_us, nats_end_us);
    return;
  }

  if (!consume_span_id.empty()) {
    nats_parent_id = consume_span_id;
  }

  if (m_ctx.m_tracer && !trace_id.empty()) {
    nlohmann::json attrs = {
        {"nats.success", true},
        {"nats.destination", m_ctx.m_config.m_db_query_nats_subject},
        {"nats.response_size", reply.m_data.size()},
        {"nats.duration_us", nats_end_us - nats_start_us},
        {"db.name", db_name},
    };
    JaegerSpanLogger::log_nats_span(m_ctx.m_tracer.get(), "NATS_db_request",
                                    200, request_id, trace_id,
                                    nats_db_span_id, nats_parent_id,
                                    nats_start_us, attrs);
  }

  const auto envelope = JsonUtils::try_parse(reply.m_data);
  if (!envelope || !envelope->is_object()) {
    send_db_gateway_error(res, 502, "BAD_GATEWAY",
                          "Invalid response envelope from DB worker", method,
                          path, start_us, trace_ctx, request_id);
    record_db_metrics(502);
    return;
  }
  const int status =
      JsonUtils::safe_get_int(*envelope, DbQueryContract::kStatus, 502);
  json response_body = json::object();
  if (envelope->contains(DbQueryContract::kBody)) {
    response_body = (*envelope)[DbQueryContract::kBody];
  }
  res.status = status;
  send_json_response(res, res.status, response_body);

  observe_db_request_duration(
      m_ctx.m_proxy.m_metrics->m_db_nats_request_duration_seconds, db_name,
      nats_start_us, nats_end_us);
  record_db_metrics(status);

  JaegerSpanLogger::log_proxy_response(
      m_ctx.m_tracer.get(), method, path, status, start_us,
      get_current_timestamp_us(), trace_ctx, m_ctx.m_config.m_mode,
      request_id);
}

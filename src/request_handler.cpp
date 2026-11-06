#include "request_handler.hpp"
#include "app_context.hpp"
#include "common_utils.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "db_gateway_routing.hpp"
#include "error_categorizer.hpp"
#include "json_utils.hpp"
#include "json_response_utils.hpp"
#include "logger.hpp"
#include "url_utils.hpp"
#include <nlohmann/json.hpp>

RequestHandler::RequestHandler(AppContext &ctx, StatsLogger &stats_logger)
    : m_ctx(ctx), m_stats_logger(stats_logger),
      m_config{.m_request_timeout_seconds =
                   ctx.m_config.m_proxy.m_request_timeout_seconds},
      m_services(ctx) {}

void RequestHandler::handle_get(const httplib::Request &req,
                                httplib::Response &res) {
  // Liveness probe - just check if process is running
  if (is_liveness_path(req.path)) {
    set_health_alive(res, "l2-proxy");
    return;
  }

  if (req.path == "/stats") {
    handle_stats(req, res);
    return;
  }

  if (req.path == "/crash-test") {
    handle_crash_test(req, res);
    return;
  }

  if (req.path == "/debug/stacktrace") {
    handle_stacktrace(res);
    return;
  }

  if (req.path == kHealthReadyPath) {
    handle_health_ready(res);
    return;
  }

  if (req.path == "/debug/duplicates") {
    handle_duplicates(res);
    return;
  }

  if (req.path.starts_with(db_gateway_routing::kDbGatewayPath)) {
    handle_db_gateway(req, res, "GET", "");
    return;
  }

  handle_request(req, res, "GET", "");
}

void RequestHandler::handle_post(const httplib::Request &req,
                                 httplib::Response &res) {
  const auto &body = req.body;
  increment_and_log_request_received(m_ctx.m_proxy.m_metrics->m_bytes_received,
                                     "Proxy", body.size());

  // Record request size in histogram
  m_ctx.m_proxy.m_metrics->m_request_size_bytes.Observe(
      static_cast<double>(body.size()));

  // Validate that the body is valid JSON (lightweight syntax check without DOM
  // allocation). The DB gateway is dispatched first: it owns its body parsing
  // and request metrics, so its 400s (invalid/empty JSON, bad contract) are
  // counted in l2_proxy_db_requests_total instead of the generic client-error
  // path.
  if (req.path.starts_with(db_gateway_routing::kDbGatewayPath)) {
    handle_db_gateway(req, res, "POST", body);
    return;
  }

  // Real FlylinkDC++ test-port traffic carries zlib-compressed bodies; with
  // raw pass-through enabled they are forwarded verbatim (base64 over NATS)
  // instead of being rejected as invalid JSON.
  if (!body.empty() && !nlohmann::json::accept(body) &&
      !m_ctx.m_config.m_proxy.m_raw_body_passthrough) {
    count_rejection(error_categorizer::RejectionReason::INVALID_JSON);
    fail_request_categorized(res, http_status::kBadRequest,
                             "Invalid JSON in request body",
                             &m_ctx.m_proxy.m_metrics->m_client_errors);
    return;
  }

  handle_request(req, res, "POST", body);
}

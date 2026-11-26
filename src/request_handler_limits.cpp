#include "request_handler.hpp"
#include "app_context.hpp"
#include "common_utils.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "duplicate_detector.hpp"
#include "dynamic_labeled_family.hpp"
#include "error_categorizer.hpp"
#include "header_utils.hpp"
#include "http_client.hpp"
#include "json_response_utils.hpp"
#include "logger.hpp"
#include "rate_limiter.hpp"
#include "rate_limiter_per_ip.hpp"
#include "tracing_helpers.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include <prometheus/counter.h>

// ============================================================================
// Phase 1: Rate limiting (global + per-IP)
// ============================================================================
bool RequestHandler::check_rate_limits(const std::string &client_ip,
                                       const std::string &client_id,
                                       const httplib::Request &req,
                                       httplib::Response &res) {
  // Trace context is extracted here (before setup_tracing) so that 429
  // rejections get their own span in Jaeger.
  const auto traceparent_raw = get_traceparent_header(req.headers);

  // Check global rate limiter first
  if (m_ctx.m_proxy.m_rate_limiter &&
      !m_ctx.m_proxy.m_rate_limiter->acquire()) {
    count_rejection(error_categorizer::RejectionReason::RATE_LIMIT_GLOBAL);
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
    count_rejection(error_categorizer::RejectionReason::RATE_LIMIT_PER_IP);
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
// Duplicate POST detection
// ============================================================================
bool RequestHandler::record_and_maybe_reject_duplicate(
    const std::string &client_id, const std::string &client_ip,
    const std::string &body, httplib::Response &res) const {
  if (!m_ctx.m_proxy.m_duplicate_detector) {
    return false;
  }
  const auto body_hash = compute_sha256_hex(body);
  if (!m_ctx.m_proxy.m_duplicate_detector
           ->record(client_id, client_ip, body_hash, body)
           .first) {
    return false;
  }
  m_ctx.m_proxy.m_metrics->m_duplicate_posts_detected.Increment();
  increment_per_client_metric(
      m_ctx.m_proxy.m_per_client_id_duplicate_collector, client_id, 0);

  // When enabled, reject the duplicate instead of forwarding it to the
  // worker: the body was already delivered within the TTL window, so
  // forwarding would only repeat a side effect. Off by default — the proxy
  // then still counts and reports the duplicate (see /debug/duplicates and
  // l2_proxy_per_client_id_duplicate_* metrics).
  if (!m_ctx.m_config.m_duplicate.m_reject_enabled) {
    return false;
  }
  increment_per_client_metric(
      m_ctx.m_proxy.m_per_client_id_duplicate_collector, client_id, 1);
  count_rejection(error_categorizer::RejectionReason::DUPLICATE);
  res.status = http_status::kConflict;
  send_json_response(res, res.status, make_error_json("duplicate request"));
  return true;
}

void RequestHandler::count_rejection(
    error_categorizer::RejectionReason reason) const {
  m_ctx.m_proxy.m_metrics->m_rejections
      .Add({{"reason", error_categorizer::rejection_reason_to_string(reason)}})
      .Increment();
}

void RequestHandler::increment_per_client_metric(
    const std::shared_ptr<DynamicLabeledFamily<prometheus::Counter>>
        &collector,
    const std::string &client_id, uint64_t bucket) {
  if (collector) {
    collector->get(client_id, bucket)->Increment();
  }
}

bool RequestHandler::reject_rate_limited(
    httplib::Response &res, const std::string &client_ip,
    const std::string &client_id, const std::string &traceparent_raw,
    const std::string &reason, prometheus::Counter &rejected_counter,
    const std::string &message, uint64_t limit, uint64_t remaining) const {
  rejected_counter.Increment();
  increment_per_client_metric(m_ctx.m_proxy.m_per_client_id_metrics_collector,
                              client_id, 1);
  res.set_header("Retry-After", "1");
  res.set_header("X-RateLimit-Limit", std::to_string(limit));
  res.set_header("X-RateLimit-Remaining", std::to_string(remaining));
  RateLimitSpanLogger::log_rate_limit_rejection(
      m_ctx.m_tracer.get(), reason, client_ip, traceparent_raw,
      std::to_string(limit), std::to_string(remaining));
  return fail_request_categorized(res, http_status::kTooManyRequests, message,
                                  &m_ctx.m_proxy.m_metrics->m_client_errors);
}

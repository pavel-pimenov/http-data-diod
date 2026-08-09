#include "nats_poll_service.hpp"
#include "common_utils.hpp"
#include "exceptions.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "nats_client.hpp"
#include "time_utils.hpp"
#include "tracing_helpers.hpp"
#include <chrono>
#include <thread>

NatsPollService::NatsPollService(AppContext &ctx) : m_ctx(ctx) {}

std::string NatsPollService::poll_response(const std::string &request_id,
                                           const std::string &request_json,
                                           int timeout_seconds,
                                           const TraceContext &trace_ctx) {
  const int64_t nats_poll_start = TimeUtils::epoch_us();
  const std::string op_span_id =
      JaegerSpanLogger::generate_span_id(m_ctx.m_tracer.get());
  std::string parent_id =
      resolve_parent_id(trace_ctx.m_parent_id, trace_ctx.m_span_id);
  Logger::debug("Starting NATS poll for response for request_id: {}",
                request_id);

  if (!m_ctx.m_nats_client) {
    Logger::error("NATS client not initialized");
    m_ctx.m_proxy.m_metrics->m_nats_connection_errors.Increment();

    JaegerSpanLogger::log_nats_span(m_ctx.m_tracer.get(), "NATS_poll", 500,
                                    request_id, trace_ctx.m_trace_id,
                                    op_span_id, parent_id, nats_poll_start,
                                    {{"nats.client_not_initialized", true}});
    return "";
  }

  if (!m_ctx.m_nats_client->is_connected()) {
    Logger::warn(
        "NATS client disconnected before poll, attempting reconnect...");
    m_ctx.m_proxy.m_metrics->m_nats_connection_errors.Increment();
  }

  try {
    Logger::debug("poll_response: using provided request_json for request_id: "
                  "{}, size: {}",
                  request_id, request_json.size());

    Logger::debug("poll_response: initial parent_id: {}", parent_id);

    int timeout_ms = timeout_seconds * 1000;
    if (timeout_ms <= 0) {
      timeout_ms = 30000;
    }

    const auto request_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout_ms);
    NatsReply reply;
    RetryHandler reconnect_backoff(250, 2000);
    bool reconnect_logged = false;
    bool no_responders_logged = false;
    bool resend_logged = false;
    bool first_attempt = true;
    int no_responders_retry_delay_ms = 1000;

    while (std::chrono::steady_clock::now() < request_deadline) {
      if (!m_ctx.m_nats_client->is_connected()) {
        if (!reconnect_logged) {
          Logger::error(
              "Proxy lost connection to NATS while waiting for response, "
              "request_id={}. Waiting for server recovery...",
              request_id);
          reconnect_logged = true;
        }

        m_ctx.m_proxy.m_metrics->m_nats_connection_errors.Increment();

        if (!m_ctx.m_nats_client->connect()) {
          const int delay_ms = reconnect_backoff.get_current_delay_ms();
          Logger::warn("Proxy reconnect to NATS failed for request_id={}, "
                       "retry in {} ms. Last error: {}",
                       request_id, delay_ms,
                       m_ctx.m_nats_client->get_last_error().value_or(""));
          std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
          reconnect_backoff.record_failure();
          continue;
        }

        Logger::info("Proxy reconnected to NATS successfully for request_id={}",
                     request_id);
        // Count real connection (re)establishments only, not the retry loop
        // iterations. Incrementing per-iteration inflated the Prometheus
        // metric with false connection churn (see 3544b39).
        m_ctx.m_proxy.m_metrics->m_nats_connection_creates.Increment();
        reconnect_backoff.record_success();
      }

      const auto now = std::chrono::steady_clock::now();
      const auto remaining_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              request_deadline - now)
              .count();
      if (remaining_ms <= 0) {
        break;
      }

      if (!first_attempt) {
        // First response was lost (empty reply / no responders / reconnect), so
        // the proxy re-sends the request/reply. The worker answers from its
        // dedup cache, so the L2 server is not called twice.
        m_ctx.m_proxy.m_metrics->m_duplicate_requests_total.Increment();
        if (!resend_logged) {
          Logger::warn(
              "Proxy re-sending NATS request/reply for request_id={} after "
              "losing the first response (worker will serve from dedup cache)",
              request_id);
          resend_logged = true;
        }
      }
      first_attempt = false;

      reply = m_ctx.m_nats_client->request_with_headers(
          m_ctx.m_config.m_nats_subject, request_json, {},
          {NatsContract::kConsumeSpanIdHeader}, static_cast<int>(remaining_ms));

      if (!reply.m_data.empty()) {
        break;
      }

      const std::string last_error =
          m_ctx.m_nats_client->get_last_error().value_or("");
      if (last_error.find("No responders available for request") !=
          std::string::npos) {
        if (!no_responders_logged) {
          Logger::warn("NATS has no responders yet for request_id={}. Waiting "
                       "for worker subscription to recover...",
                       request_id);
          no_responders_logged = true;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(no_responders_retry_delay_ms));
      } else {
        Logger::warn("NATS request returned empty response for request_id={}, "
                     "will retry while timeout budget remains. Last error: {}",
                     request_id, last_error);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
    }

    if (reply.m_data.empty()) {
      Logger::error("Failed to get response from NATS for request_id={} before "
                    "timeout; last error={}",
                    request_id,
                    m_ctx.m_nats_client->get_last_error().value_or(""));

      JaegerSpanLogger::log_nats_span(
          m_ctx.m_tracer.get(), "NATS_poll", 500, request_id,
          trace_ctx.m_trace_id, op_span_id, parent_id, nats_poll_start,
          {{"nats.empty_response", true},
           {"nats.last_error",
            m_ctx.m_nats_client->get_last_error().value_or("")}});
      return "";
    }

    Logger::debug("Received NATS response for request_id: {}, size: {}",
                  request_id, reply.m_data.size());

    // Read nats_consume_span_id from NATS headers (no JSON parse needed)
    const auto it_header =
        reply.m_headers.find(NatsContract::kConsumeSpanIdHeader);
    if (it_header != reply.m_headers.end() && !it_header->second.empty()) {
      parent_id = it_header->second;
      Logger::debug(
          "poll_response: using nats_consume_span_id from header as parent: {}",
          parent_id);
    }

    const int64_t duration_us = TimeUtils::epoch_us() - nats_poll_start;
    m_ctx.m_proxy.m_metrics->m_nats_request_duration_seconds.Observe(
        duration_us / 1000000.0);

    JaegerSpanLogger::log_nats_span(
        m_ctx.m_tracer.get(), "NATS_poll", 200, request_id,
        trace_ctx.m_trace_id, op_span_id, parent_id, nats_poll_start,
        {{"nats.response_size", reply.m_data.size()},
         {"nats.duration_us", duration_us}});

    return reply.m_data;

  } catch (const std::exception &e) {
    Logger::error("NATS poll exception: {}", e.what());
    m_ctx.m_proxy.m_metrics->m_nats_errors.Increment();

    JaegerSpanLogger::log_nats_span(m_ctx.m_tracer.get(), "NATS_poll", 500,
                                    request_id, trace_ctx.m_trace_id,
                                    op_span_id, parent_id, nats_poll_start,
                                    {{"nats.error", e.what()}});
    return "";
  }
}

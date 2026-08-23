#ifndef TRACING_HELPERS_HPP
#define TRACING_HELPERS_HPP

#include "app_context.hpp"
#include "common_utils.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "nlohmann/json.hpp"
#include "time_utils.hpp"
#include "trace_logger.hpp"
#include <cstdint>
#include <string>

// Reads the "traceparent" header value from an httplib header map, or an empty
// string when absent. Shared by the trace-context extraction helpers and the
// rate-limit rejection path (which needs the raw header before setup_tracing).
inline std::string get_traceparent_header(const httplib::Headers &headers) {
  const auto traceparent_it = headers.find("traceparent");
  return (traceparent_it != headers.end()) ? traceparent_it->second : "";
}

// Forward declaration: defined further below, but used by begin_request_trace.
inline std::string log_incoming_span(JaegerLogger *tracer,
                                    const std::string &path, uint64_t start_us,
                                    const std::string &request_id,
                                    const TraceContext &trace_ctx);

// Shared proxy trace-start for an inbound HTTP request: extract the traceparent,
// build the TraceContext, set the thread-local trace_id and log the INCOMING
// span. The client IP and request_id are set by the caller (client IP via
// ScopedRequestContext, request_id already generated). Returns the TraceContext
// and fills inlet_span_id. Mirrors the prologue duplicated between the main
// request path and the DB-gateway path.
inline TraceContext begin_request_trace(JaegerLogger *tracer,
                                      const httplib::Headers &headers,
                                      const std::string &request_id,
                                      const std::string &path,
                                      uint64_t start_us,
                                      std::string &inlet_span_id) {
  const std::string traceparent_raw = get_traceparent_header(headers);
  const TraceContext trace_ctx = handle_trace_context(traceparent_raw, tracer);
  Logger::set_trace_id(trace_ctx.m_trace_id);
  inlet_span_id =
      log_incoming_span(tracer, path, start_us, request_id, trace_ctx);
  return trace_ctx;
}

// Service-name prefix used as the Jaeger operation/service label, e.g.
// "l2-proxy-proxy" / "l2-proxy-worker". Centralises the "l2-proxy-" prefix so
// span loggers don't recompose it by hand in each call site.
inline std::string proxy_service_name(const std::string &mode) {
  return "l2-proxy-" + mode;
}

class TraceContextHelper {
public:
  static TraceContext extract_and_validate(const httplib::Headers &headers,
                                           JaegerLogger *tracer,
                                           const std::string &context) {
    return extract_from_raw(get_traceparent_header(headers), tracer, context);
  }

  static TraceContext extract_from_raw(const std::string &traceparent_raw,
                                       JaegerLogger *tracer,
                                       const std::string &context) {
    if (!traceparent_raw.empty()) {
      Logger::debug("{} - traceparent_raw: {}", context, traceparent_raw);
    } else {
      Logger::warn("{} - traceparent_raw: not found", context);
    }

    const TraceContext trace_ctx =
        handle_trace_context(traceparent_raw, tracer);
    Logger::debug("{} span_id: {}", context, trace_ctx.m_span_id);

    validate_trace_context(trace_ctx, context);

    return trace_ctx;
  }

private:
  static void validate_trace_context(const TraceContext &ctx,
                                     const std::string &context) {
    if (ctx.m_trace_id.empty()) {
      Logger::error("{} - Invalid trace context: empty trace_id", context);
    }
    if (ctx.m_span_id.empty()) {
      Logger::error("{} - Invalid trace context: empty span_id", context);
    }
  }
};

class JaegerSpanLogger {
public:
  static void
  log_l2_call(JaegerLogger *tracer, const std::string &method,
              const std::string &url, int status_code, uint64_t start_us,
              uint64_t end_us, const TraceContext &trace_ctx,
              const std::string &mode, const std::string &span_id,
              const std::string &parent_span_id,
              const std::string &request_id = "",
              const nlohmann::json &attrs = nlohmann::json::object()) {
    if (!tracer)
      return;

    tracer->log_request(method, url, status_code, start_us, end_us,
                        proxy_service_name(mode) + "-call-l2-server",
                        request_id, trace_ctx.m_trace_id, span_id,
                        parent_span_id, attrs);
  }

  static void log_worker_processing(
      JaegerLogger *tracer, const std::string &method, const std::string &path,
      int status_code, uint64_t start_us, uint64_t end_us,
      const TraceContext &trace_ctx, const std::string &mode,
      const std::string &span_id, const std::string &request_id) {
    if (!tracer)
      return;

    tracer->log_request(method, path, status_code, start_us, end_us,
                        proxy_service_name(mode) + "-process-request",
                        request_id, trace_ctx.m_trace_id, span_id,
                        trace_ctx.m_parent_id, {});
  }

  static void log_proxy_response(JaegerLogger *tracer,
                                 const std::string &method,
                                 const std::string &path, int status_code,
                                 uint64_t start_us, uint64_t end_us,
                                 const TraceContext &trace_ctx,
                                 const std::string &mode,
                                 const std::string &request_id) {
    if (!tracer)
      return;

    tracer->log_request(method, path, status_code, start_us, end_us,
                        proxy_service_name(mode), request_id,
                        trace_ctx.m_trace_id, trace_ctx.m_span_id,
                        trace_ctx.m_parent_id, {});
  }

  static std::string generate_span_id(JaegerLogger *tracer) {
    return tracer ? tracer->generate_span_id() : "";
  }

  static void
  log_nats_span(JaegerLogger *tracer, const std::string &operation,
                int status_code, const std::string &request_id,
                const std::string &trace_id, const std::string &span_id,
                const std::string &parent_id, int64_t start_us,
                const nlohmann::json &extra_attrs = nlohmann::json::object()) {
    if (!tracer)
      return;

    const int64_t end_us = TimeUtils::epoch_us();
    nlohmann::json attrs = extra_attrs;
    attrs["nats.success"] = nlohmann::json(status_code == 200);
    tracer->log_request(operation, "/nats", status_code, start_us, end_us,
                        "l2-proxy-NATS", request_id, trace_id, span_id,
                        parent_id, attrs);
  }
};

class BackendErrorSpanLogger {
public:
  // Log a Jaeger span for a backend (worker/NATS) failure surfaced to the
  // client as a 5xx response. The INCOMING span is logged — with a hardcoded
  // 200 status — before the request outcome is known, so without this span a
  // failed request would look successful in traces. Uses the same convention
  // as log_proxy_response: span_id = derived proxy span, parent = the client
  // span from the traceparent header, if any.
  static void
  log_backend_error(JaegerLogger *tracer, const std::string &method,
                    const std::string &path, int status_code, uint64_t start_us,
                    const TraceContext &trace_ctx, const std::string &mode,
                    const std::string &request_id, const std::string &category,
                    const std::string &detail) {
    if (!tracer) {
      return;
    }

    const uint64_t now_us = get_current_timestamp_us();
    nlohmann::json attrs = {
        {"backend.error", category},
    };
    if (!detail.empty()) {
      attrs["backend.detail"] = detail;
    }

    tracer->log_request(method, path, status_code, start_us, now_us,
                        proxy_service_name(mode), request_id,
                        trace_ctx.m_trace_id, trace_ctx.m_span_id,
                        trace_ctx.m_parent_id, attrs);
  }
};

class RateLimitSpanLogger {
public:
  // Log a Jaeger span for an HTTP 429 rejection. Rate limiting runs before
  // setup_tracing(), so the trace context is extracted (or generated) here —
  // otherwise rate-limit rejections would be invisible in traces.
  static void log_rate_limit_rejection(JaegerLogger *tracer,
                                       const std::string &reason,
                                       const std::string &client_ip,
                                       const std::string &traceparent_header,
                                       const std::string &limit,
                                       const std::string &remaining) {
    if (!tracer) {
      return;
    }

    const uint64_t now_us = get_current_timestamp_us();
    const TraceContext trace_ctx =
        handle_trace_context(traceparent_header, tracer);

    nlohmann::json attrs = {
        {"rate_limit.reason", reason},
        {"rate_limit.client_ip", client_ip},
    };
    if (!limit.empty()) {
      attrs["rate_limit.limit"] = limit;
    }
    if (!remaining.empty()) {
      attrs["rate_limit.remaining"] = remaining;
    }

    // The rejection span doubles as the proxy span for the request (same
    // convention as log_proxy_response: span_id = derived proxy span, parent
    // = the client span from the traceparent header, if any).
    tracer->log_request("POST", "/", 429, now_us, now_us, "l2-proxy-proxy", "",
                        trace_ctx.m_trace_id, trace_ctx.m_span_id,
                        trace_ctx.m_parent_id, attrs);
  }
};

inline std::string resolve_trace_id(JaegerLogger *tracer,
                                    const TraceContext &ctx) {
  if (!ctx.m_trace_id.empty()) {
    return ctx.m_trace_id;
  }
  return tracer ? tracer->generate_trace_id() : "";
}

// Generates a fresh span id (or reuses span_id_hint) plus the traceparent
// string referencing it under ctx's trace id. Shared by the proxy's
// trace-context extraction, the NATS push path and the worker's L2 call, which
// each rebuilt the "new span id + matching traceparent" pair by hand.
inline std::pair<std::string, std::string>
make_span_and_traceparent(JaegerLogger *tracer, const TraceContext &ctx,
                          const std::string &span_id_hint = "",
                          bool sampled = true) {
  const std::string span_id =
      span_id_hint.empty() ? JaegerSpanLogger::generate_span_id(tracer)
                           : span_id_hint;
  if (!tracer || ctx.m_trace_id.empty()) {
    return {span_id, ctx.m_traceparent_header};
  }
  return {span_id,
          tracer->generate_traceparent(ctx.m_trace_id, span_id, sampled)};
}

// Logs the proxy INCOMING span and returns the generated inlet span id. Shared
// by the main request handler and the DB gateway, which duplicated the
// resolve-trace-id + generate-span-id + log_request sequence.
inline std::string log_incoming_span(JaegerLogger *tracer,
                                     const std::string &path,
                                     uint64_t start_us,
                                     const std::string &request_id,
                                     const TraceContext &trace_ctx) {
  if (!tracer) {
    return "";
  }
  const std::string trace_id = resolve_trace_id(tracer, trace_ctx);
  const std::string span_id = tracer->generate_span_id();
  tracer->log_request("INCOMING", path, 200, start_us, start_us,
                      "l2-proxy-proxy", request_id, trace_id, span_id,
                      trace_ctx.m_parent_id, nlohmann::json({}));
  return span_id;
}

// Attaches the proxy's tracing context (trace_id/span_id/inlet_span_id/
// traceparent) to a request forwarded to the worker. Shared by the main
// request path and the DB gateway, which wrote the same keys in three places.
inline void add_proxy_trace_fields(json &request_data, JaegerLogger *tracer,
                                   const TraceContext &trace_ctx,
                                   const std::string &inlet_span_id,
                                   const std::string &span_id,
                                   bool sampled = true) {
  if (!tracer) {
    return;
  }
  const std::string trace_id = resolve_trace_id(tracer, trace_ctx);
  request_data[NatsContract::kProxyTraceId] = trace_id;
  request_data[NatsContract::kProxySpanId] = span_id;
  request_data[NatsContract::kProxyInletSpanId] = inlet_span_id;
  request_data[NatsContract::kProxyTraceparent] =
      tracer->generate_traceparent(trace_id, span_id, sampled);
}

// Sets the "traceparent" response header from the request's trace context.
// Shared by the l2-server and the proxy response builder, which duplicated the
// set_header call (with differing empty-check).
inline void set_traceparent_response_header(
    httplib::Response &res, const TraceContext &trace_ctx) {
  if (!trace_ctx.m_traceparent_header.empty()) {
    res.set_header("traceparent", trace_ctx.m_traceparent_header);
  }
}

#endif // TRACING_HELPERS_HPP

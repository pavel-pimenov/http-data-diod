#include "request_handler.hpp"
#include "app_context.hpp"
#include "common_utils.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "db_gateway_routing.hpp"
#include "db_query_utils.hpp"
#include "json_response_utils.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "metrics_manager.hpp"
#include "nats_client.hpp" // IWYU pragma: keep (NatsClient via shared_ptr needs complete type)
#include "time_utils.hpp"
#include "trace_context_extractor.hpp"
#include "trace_logger.hpp"
#include "tracing_helpers.hpp"
#include "url_utils.hpp"
#include <algorithm>
#include <format>
#include <string>
#include <nlohmann/json.hpp>

// Maps an unknown-db error into a ready-to-send HTTP response.
void send_db_error(httplib::Response &res, int status, const std::string &code,
                   const std::string &message) {
  res.status = status;
  res.set_content(make_db_error_body(code, message).dump(),
                  "application/json");
}

void RequestHandler::handle_db_gateway(const httplib::Request &req,
                                       httplib::Response &res,
                                       const std::string &method,
                                       const std::string &body) {
  // Distributed tracing by analogy with the main request path: extract (or
  // generate) the trace context, log the INCOMING span and correlate all DB
  // gateway log lines via the thread-local request context.
  const uint64_t start_us = get_current_timestamp_us();
  const auto request_id = m_services.m_id_generator.generate_uuid();

  ScopedRequestContext const req_ctx(req);
  Logger::set_request_id(request_id);
  std::string inlet_span_id;
  const TraceContext trace_ctx = begin_request_trace(
      m_ctx.m_tracer.get(), req.headers, request_id, req.path, start_us,
      inlet_span_id);

  // Attach the proxy's tracing context to the DB request so the worker can
  // extend the same trace (analogous to NatsPushService on the main path).
  auto add_trace_fields = [&](json &request_json) {
    if (!m_ctx.m_tracer) {
      return;
    }
    const auto nats_db_span_id = JaegerLogger::generate_span_id();
    add_proxy_trace_fields(request_json, m_ctx.m_tracer.get(), trace_ctx,
                           inlet_span_id, nats_db_span_id,
                           trace_ctx.m_sampled);
  };

  // Combined error path used by every pre-routing rejection: logs the Jaeger
  // proxy-response span, writes the JSON error body and bumps the counter.
  // The counter is the same one route_db_request uses, so pre-routing failures
  // (404/405/400) are counted alongside the NATS-routed requests; these paths
  // never reach NATS, so no duration histogram is observed for them.
  auto reject_gateway = [&](int status, const std::string &code,
                            const std::string &message, const std::string &db,
                            const std::string &type) {
    send_db_gateway_error(res, status, code, message, method, req.path,
                          start_us, trace_ctx, request_id);
    record_db_request_metrics(m_ctx.m_proxy.m_metrics->m_db_requests_total,
                              db, type, status);
  };

  if (!m_ctx.m_config.m_db_query.m_enabled) {
    reject_gateway(http_status::kNotFound, "NOT_FOUND",
                   "DB gateway is disabled", "", "");
    return;
  }

  const auto parsed = db_gateway_routing::parse_path(
      db_gateway_routing::normalize_path_rest(
          req.path.substr(std::string(db_gateway_routing::kDbGatewayPath).size())));

  if (parsed.m_is_list) {
    handle_db_gateway_list(res, method, req.path, start_us, trace_ctx,
                           request_id);
    return;
  }

  const auto &db_name = parsed.m_db_name;
  const auto &action = parsed.m_action;
  if (!parsed.m_valid) {
    reject_gateway(http_status::kNotFound, "NOT_FOUND",
                   "Unknown DB gateway path", db_name, action);
    return;
  }

  const bool known_db = std::ranges::any_of(
      m_ctx.m_config.m_db_query.m_databases,
      [&db_name](const DbConfig &db) { return db.m_name == db_name; });
  if (!known_db) {
    reject_gateway(http_status::kNotFound, "UNKNOWN_DATABASE",
                   std::format("Unknown database '{}'", db_name),
                   db_name, action);
    return;
  }

  if (action == "query" && method == "POST") {
    const auto parsed_body = JsonUtils::try_parse(body);
    if (!parsed_body) {
      reject_gateway(http_status::kBadRequest, "BAD_REQUEST",
                     "Invalid JSON body", db_name, action);
      return;
    }
    json request = build_db_query_request(DbQueryContract::kTypeQuery,
                                          request_id, db_name, *parsed_body);
    add_trace_fields(request);
    const auto validated = parse_db_query_request(request);
    if (!validated) {
      reject_gateway(http_status::kBadRequest, "BAD_REQUEST",
                     validated.error(), db_name, action);
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

  // Known action with the wrong HTTP method → 405 Method Not Allowed (an
  // action exists but the client used the wrong verb); unknown actions → 404.
  // The decision is computed by the testable db_gateway_routing helpers.
  const auto method_decision =
      db_gateway_routing::classify_method(action, method);
  if (method_decision.m_is_error) {
    reject_gateway(method_decision.m_status, method_decision.m_code,
                   method_decision.m_message, db_name, action);
    return;
  }
}

void RequestHandler::handle_db_gateway_list(
    httplib::Response &res, const std::string &method,
    const std::string &path, uint64_t start_us, const TraceContext &trace_ctx,
    const std::string &request_id) {
  if (method != "GET") {
    send_db_gateway_error(res, http_status::kMethodNotAllowed,
                          "METHOD_NOT_ALLOWED", "Use GET", method, path,
                          start_us, trace_ctx, request_id);
    record_db_request_metrics(m_ctx.m_proxy.m_metrics->m_db_requests_total, "",
                              "list", http_status::kMethodNotAllowed);
    return;
  }
  json names = db_gateway_routing::databases_list_json(m_ctx.m_config.m_db_query.m_databases);
  res.status = http_status::kOk;
  send_json_response(res, res.status, json{{"databases", names}});
  record_db_request_metrics(m_ctx.m_proxy.m_metrics->m_db_requests_total, "",
                            "list", http_status::kOk);
}

void RequestHandler::send_db_gateway_error(
    httplib::Response &res, int status, const std::string &code,
    const std::string &message, const std::string &method,
    const std::string &path, uint64_t start_us, const TraceContext &trace_ctx,
    const std::string &request_id) const {
  if (m_ctx.m_tracer) {
    JaegerSpanLogger::log_proxy_response(
        m_ctx.m_tracer.get(), method, path, status, start_us,
        get_current_timestamp_us(), trace_ctx, m_ctx.m_config.m_app.m_mode,
        request_id);
  }
  send_db_error(res, status, code, message);
}

void RequestHandler::route_db_request(
    httplib::Response &res, const json &request, const std::string &method,
    const std::string &path, uint64_t start_us, const TraceContext &trace_ctx,
    const std::string &request_id, const std::string &inlet_span_id) {
  const auto db_name =
      JsonUtils::safe_get_string(request, DbQueryContract::kDb);

  // Records the gateway counters/histograms on every exit path (success and
  // each failure branch) so a stuck/erroring DB gateway is visible in
  // Prometheus, not just in traces.
  auto record_db_metrics = [this, &request, &db_name, start_us](int status) {
    const auto type =
        JsonUtils::safe_get_string(request, DbQueryContract::kType);
    record_db_request_metrics(m_ctx.m_proxy.m_metrics->m_db_requests_total,
                             db_name, type, status);
    observe_db_request_duration(
        m_ctx.m_proxy.m_metrics->m_db_request_duration_seconds, db_name,
        start_us, get_current_timestamp_us());
  };

  // Combined error path for post-routing NATS failures: Jaeger panic log,
  // JSON error body and the DB duration counters, one call instead of the
  // send_db_gateway_error + record_db_metrics pair repeated after each branch.
  auto reject_db_request = [this, &res, &record_db_metrics, method, path,
                            start_us, &trace_ctx, request_id](
                               int status, const std::string &message) {
    std::string code;
    switch (status) {
      case http_status::kServiceUnavailable:
        code = "DB_UNAVAILABLE";
        break;
      case http_status::kGatewayTimeout:
        code = "TIMEOUT";
        break;
      default:
        code = "BAD_GATEWAY";
    }
    send_db_gateway_error(res, status, code, message, method, path, start_us,
                          trace_ctx, request_id);
    record_db_metrics(status);
  };

  if (!m_ctx.m_nats_client) {
    reject_db_request(http_status::kServiceUnavailable,
                      "NATS client is not available");
    return;
  }

  const auto request_json = request.dump();
  const int64_t nats_start_us = TimeUtils::epoch_us();
  const auto trace_id = resolve_trace_id(m_ctx.m_tracer.get(), trace_ctx);
  auto nats_db_span_id =
      JsonUtils::safe_get_string(request, NatsContract::kProxySpanId);
  auto nats_parent_id =
      resolve_parent_id(inlet_span_id, trace_ctx.m_parent_id);

  // Records the NATS round-trip span in both outcomes (empty reply vs. reply
  // received); the attribute set is the same in reversed form in each branch.
  auto log_db_nats_roundtrip = [&](bool success,
                                   const std::string &last_error,
                                   size_t response_size,
                                   int64_t nats_end_us) {
    if (!m_ctx.m_tracer || trace_id.empty()) {
      return;
    }
    nlohmann::json attrs = {
        {"nats.success", success},
        {"nats.destination", m_ctx.m_config.m_db_query.m_subject},
        {"nats.duration_us", nats_end_us - nats_start_us},
        {"db.name", db_name},
    };
    if (success) {
      attrs["nats.response_size"] = response_size;
    } else if (!last_error.empty()) {
      attrs["nats.last_error"] = last_error;
    }
    JaegerSpanLogger::log_nats_span(
        m_ctx.m_tracer.get(), "NATS_db_request",
        success ? http_status::kOk : http_status::kInternalServerError,
        request_id, trace_id, nats_db_span_id, nats_parent_id, nats_start_us,
        attrs);
  };

  // Records the NATS round-trip latency histogram on both outcomes (empty
  // reply vs. reply received), which previously duplicated the same
  // observe_db_request_duration call in the failure and success branches.
  auto record_db_nats_duration = [this, &db_name,
                                  nats_start_us](int64_t nats_end_us) {
    observe_db_request_duration(
        m_ctx.m_proxy.m_metrics->m_db_nats_request_duration_seconds, db_name,
        nats_start_us, nats_end_us);
  };

  // request_with_consume_span_id also returns the worker's consume span id
  // (NATS header), which links the round-trip span to the worker's consume
  // span.
  auto [reply, consume_span_id] =
      m_ctx.m_nats_client->request_with_consume_span_id(
          m_ctx.m_config.m_db_query.m_subject, request_json,
          m_ctx.m_config.m_db_query.m_timeout_ms);
  const int64_t nats_end_us = TimeUtils::epoch_us();

  if (reply.m_data.empty()) {
    const auto last_error =
        m_ctx.m_nats_client->get_last_error().value_or("");
    log_db_nats_roundtrip(false, last_error, 0, nats_end_us);
    if (!m_ctx.m_nats_client->is_connected()) {
      reject_db_request(http_status::kServiceUnavailable,
                        "NATS connection is not available");
    } else {
      reject_db_request(http_status::kGatewayTimeout,
                        "DB worker did not respond in time");
    }
    record_db_nats_duration(nats_end_us);
    return;
  }

  if (!consume_span_id.empty()) {
    nats_parent_id = consume_span_id;
  }

  log_db_nats_roundtrip(true, "", reply.m_data.size(), nats_end_us);

  const auto envelope = JsonUtils::try_parse(reply.m_data);
  if (!envelope || !envelope->is_object()) {
    reject_db_request(http_status::kBadGateway,
                      "Invalid response envelope from DB worker");
    return;
  }
  const int status =
      JsonUtils::safe_get_int(*envelope, DbQueryContract::kStatus,
                             http_status::kBadGateway);
  json response_body = json::object();
  if (envelope->contains(DbQueryContract::kBody)) {
    response_body = (*envelope)[DbQueryContract::kBody];
  }
  res.status = status;
  send_json_response(res, res.status, response_body);

  record_db_nats_duration(nats_end_us);
  record_db_metrics(status);

  JaegerSpanLogger::log_proxy_response(
      m_ctx.m_tracer.get(), method, path, status, start_us,
      get_current_timestamp_us(), trace_ctx, m_ctx.m_config.m_app.m_mode,
      request_id);
}

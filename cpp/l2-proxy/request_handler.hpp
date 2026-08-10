#ifndef REQUEST_HANDLER_HPP
#define REQUEST_HANDLER_HPP

#include "app_context.hpp"
#include "common_utils.hpp"
#include "httplib/httplib.h"
#include "nats_poll_service.hpp"
#include "nats_push_service.hpp"
#include "nlohmann/json.hpp"
#include "request_data_preparer.hpp"
#include "request_id_generator.hpp"
#include "response_builder.hpp"
#include "stats_logger.hpp"
#include "trace_context_extractor.hpp"
#include <atomic>
#include <functional>
#include <string>

using json = nlohmann::json;

class RequestHandler {
private:
  static constexpr int g_default_request_timeout_seconds = 15;
  static constexpr int g_default_retry_delay_ms = 100;
  static constexpr int g_max_retry_delay_ms = 2000;

  AppContext &m_ctx;
  StatsLogger *m_stats_logger;
  int m_request_timeout_seconds = 30;

  RequestIdGenerator m_id_generator;
  NatsPushService m_push_service;
  NatsPollService m_poll_service;

  void handle_request(const httplib::Request &req, httplib::Response &res,
                      const std::string &method, const std::string &body = "");

  // Phase 1: Rate limiting (global + per-IP)
  // Returns false if request was rejected (response already set)
  bool check_rate_limits(const std::string &client_ip,
                         const std::string &client_id,
                         const httplib::Request &req, httplib::Response &res);

  // Phase 2: Process request, push to backend, poll for response, cache result
  // Returns false on error (response already set)
  bool process_request(const std::string &method, const std::string &path,
                       const std::string &body, const httplib::Request &req,
                       httplib::Response &res, const std::string &client_ip);

  // Phase 3a: Extract trace context and create tracing spans
  TraceContext setup_tracing(const httplib::Request &req,
                             const std::string &request_id, long long start_us,
                             std::string &inlet_span_id,
                             std::string &backend_push_span_id,
                             std::string &traceparent_for_backend);

  // Phase 3b: Push request to backend (NATS)
  // Returns the serialized request JSON for use in poll_for_response()
  std::string push_to_backend(nlohmann::json request_data,
                              const std::string &request_id,
                              const std::string &trace_id,
                              const std::string &backend_push_span_id,
                              const TraceContext &trace_ctx);

  // Phase 3c: Poll for response from backend
  std::string poll_for_response(const std::string &request_id,
                                const std::string &request_json,
                                const TraceContext &trace_ctx);

  // Phase 4: Send response to client
  void send_response(httplib::Response &res,
                     const nlohmann::json &response_data,
                     const std::string &request_id,
                     const TraceContext &trace_ctx, const std::string &method,
                     const std::string &path, long long start_us);

  // Backend (NATS) failure: logs a Jaeger span for the failed backend call and
  // sets a JSON 5xx response. Returns false (the request-handler failure
  // idiom), so call sites are a single `return fail_backend_request(...)`.
  bool fail_backend_request(httplib::Response &res, const std::string &method,
                            const std::string &path, int status,
                            const std::string &message,
                            const std::string &category,
                            const std::string &detail, uint64_t start_us,
                            const TraceContext &trace_ctx,
                            const std::string &request_id);

  // HTTP 429 rejection shared by the global and per-IP rate limiters:
  // increments the rejection counters, sets Retry-After + X-RateLimit-*
  // headers, logs a Jaeger span and writes the JSON error response. Returns
  // false.
  bool reject_rate_limited(httplib::Response &res, const std::string &client_ip,
                           const std::string &client_id,
                           const std::string &traceparent_raw,
                           const std::string &reason,
                           prometheus::Counter &rejected_counter,
                           const std::string &message, uint64_t limit,
                           uint64_t remaining);

public:
  explicit RequestHandler(AppContext &ctx, StatsLogger *stats_logger = nullptr);
  ~RequestHandler();
  void handle_get(const httplib::Request &req, httplib::Response &res);
  void handle_post(const httplib::Request &req, httplib::Response &res);

private:
  // HTTP DB Gateway: routes /v1/sql/** (list/ping/query) to the worker over
  // NATS and maps the worker reply envelope back to an HTTP response.
  void handle_db_gateway(const httplib::Request &req, httplib::Response &res,
                         const std::string &method,
                         const std::string &body);
  // Sends a DB gateway error response (send_db_error) and logs the proxy
  // Jaeger span so failed /v1/sql requests stay visible in traces.
  void send_db_gateway_error(httplib::Response &res, int status,
                             const std::string &code,
                             const std::string &message,
                             const std::string &method, const std::string &path,
                             uint64_t start_us, const TraceContext &trace_ctx,
                             const std::string &request_id);
  // Sends a validated DbQueryContract request to the DB subject, logs the
  // NATS round-trip span and applies the worker's {status, body} envelope to
  // the HTTP response.
  void route_db_request(httplib::Response &res, const json &request,
                        const std::string &method, const std::string &path,
                        uint64_t start_us, const TraceContext &trace_ctx,
                        const std::string &request_id,
                        const std::string &inlet_span_id);
};

#endif // REQUEST_HANDLER_HPP

#include "nats_push_service.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "nats_client.hpp"
#include "time_utils.hpp"
#include "tracing_helpers.hpp"

NatsPushService::NatsPushService(AppContext &ctx) : m_ctx(ctx) {}

std::string NatsPushService::push_request(nlohmann::json request_data,
                                          const std::string &trace_id,
                                          const std::string &span_id,
                                          const TraceContext &trace_ctx) {
  const int64_t nats_push_start = TimeUtils::epoch_us();
  std::string op_span_id;
  if (request_data.contains(NatsContract::kProxySpanId)) {
    op_span_id = request_data[NatsContract::kProxySpanId].get<std::string>();
  } else {
    op_span_id = span_id.empty()
                     ? JaegerSpanLogger::generate_span_id(m_ctx.m_tracer.get())
                     : span_id;
  }
  std::string parent_id;
  if (request_data.contains(NatsContract::kProxyInletSpanId)) {
    parent_id =
        request_data[NatsContract::kProxyInletSpanId].get<std::string>();
  } else {
    parent_id = resolve_parent_id(trace_ctx.m_parent_id, trace_ctx.m_span_id);
  }

  const std::string request_id =
      request_data[NatsContract::kRequestId].get<std::string>();

  // Add timestamp and tracing fields directly (no deep copy needed —
  // request_data is by value)
  request_data[NatsContract::kTimestamp] = TimeUtils::epoch_ms();
  request_data[NatsContract::kProxySpanId] = op_span_id;
  request_data[NatsContract::kProxyTraceId] = trace_id;
  if (m_ctx.m_tracer && !trace_id.empty() && !op_span_id.empty()) {
    request_data[NatsContract::kProxyTraceparent] =
        m_ctx.m_tracer->generate_traceparent(trace_id, op_span_id, true);
  }

  std::string nats_request_json = request_data.dump();
  Logger::debug("NATS request serialized: request_id={} size={}", request_id,
                nats_request_json.size());

  m_ctx.m_proxy.m_metrics->m_nats_requests.Increment();

  JaegerSpanLogger::log_nats_span(
      m_ctx.m_tracer.get(), "NATS_push", 200, request_id, trace_id, op_span_id,
      parent_id, nats_push_start,
      {{"nats.request_size", nats_request_json.size()}});

  return nats_request_json;
}

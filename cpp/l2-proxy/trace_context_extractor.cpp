#include "trace_context_extractor.hpp"
#include "logger.hpp"

TraceContext extract_trace_context(const httplib::Request &req,
                                   JaegerLogger *tracer,
                                   std::string &backend_push_span_id,
                                   std::string &traceparent_for_backend) {
  const auto traceparent_it = req.headers.find("traceparent");
  const std::string traceparent_raw =
      (traceparent_it != req.headers.end()) ? traceparent_it->second : "";
  TraceContext trace_ctx = handle_trace_context(traceparent_raw, tracer);
  Logger::debug("Proxy parsed trace context: trace_id={} span_id={}",
                trace_ctx.m_trace_id, trace_ctx.m_span_id);

  // Generate span_id for backend push
  backend_push_span_id = tracer ? tracer->generate_span_id() : "";

  // Generate traceparent for backend push
  traceparent_for_backend = trace_ctx.m_traceparent_header;
  if (tracer && !trace_ctx.m_trace_id.empty()) {
    traceparent_for_backend = tracer->generate_traceparent(
        trace_ctx.m_trace_id, backend_push_span_id, true);
  }

  return trace_ctx;
}
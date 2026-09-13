#include "trace_context_extractor.hpp"
#include "logger.hpp"
#include "tracing_helpers.hpp"

TraceContext extract_trace_context(const httplib::Request &req,
                                   JaegerLogger *tracer,
                                   std::string &backend_push_span_id,
                                   std::string &traceparent_for_backend) {
  const std::string traceparent_raw = get_traceparent_header(req.headers);
  TraceContext trace_ctx = handle_trace_context(traceparent_raw, tracer);
  Logger::debug("Proxy parsed trace context: trace_id={} span_id={}",
                trace_ctx.m_trace_id, trace_ctx.m_span_id);

  // Generate a fresh span id for backend push and rebuild the traceparent
  // referencing it (the original header points at the client's span).
  std::tie(backend_push_span_id, traceparent_for_backend) =
      make_span_and_traceparent(tracer, trace_ctx);

  return trace_ctx;
}
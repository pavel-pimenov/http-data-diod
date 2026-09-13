#ifndef TRACE_CONTEXT_EXTRACTOR_HPP
#define TRACE_CONTEXT_EXTRACTOR_HPP

#include "common_utils.hpp"
#include "httplib/httplib.h"
#include "trace_logger.hpp"
#include <string>

// Extract trace context from HTTP request
TraceContext extract_trace_context(const httplib::Request &req,
                                   JaegerLogger *tracer,
                                   std::string &backend_push_span_id,
                                   std::string &traceparent_for_backend);

#endif // TRACE_CONTEXT_EXTRACTOR_HPP
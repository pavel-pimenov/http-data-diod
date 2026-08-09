#include "response_builder.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "tracing_helpers.hpp"
#include <algorithm>
#include <base64.hpp>

void set_response_content(httplib::Response &res,
                          const nlohmann::json &parsed_response_data,
                          const std::string &request_id,
                          const TraceContext &trace_ctx,
                          const std::string &method, const std::string &path,
                          uint64_t start_us, AppContext &ctx,
                          StatsLogger *stats_logger) {
  if (!parsed_response_data.is_object()) {
    handle_error("Invalid response format",
                 &ctx.m_proxy.m_metrics->m_client_errors);
    set_json_error_response(res, 500, "Invalid response format", request_id);
    return;
  }

  // Extract the actual response from l2-server without deep-copying the JSON
  // body string (parsed_response_data outlives the reference below)
  const auto &l2_response =
      parsed_response_data[NatsResponseContract::kBody]
                          [NatsResponseContract::kBodyResponse]
                              .get_ref<const std::string &>();
  const int status_code = parsed_response_data[NatsResponseContract::kStatus];
  res.status = status_code;

  // Check if response contains binary data (base64 encoded)
  const bool is_binary = JsonUtils::safe_get_bool(
      parsed_response_data[NatsResponseContract::kBody],
      NatsResponseContract::kBodyIsBinary);
  const std::string content_type =
      parsed_response_data[NatsResponseContract::kBody].value(
          NatsResponseContract::kBodyContentType, "application/json");

  // For binary data, decode from base64 into a separate buffer
  std::string decoded_binary_response;
  if (is_binary) {
    // Base64 decode using libbase64 library (faster than manual implementation)
    decoded_binary_response = base64::from_base64(l2_response);
    Logger::debug("Proxy decoded binary response, size={}",
                  decoded_binary_response.size());
  }

  increment_and_log_response_sent(ctx.m_proxy.m_metrics->m_bytes_sent, "Proxy",
                                  request_id, status_code);

  ctx.m_proxy.m_metrics->m_response_size_bytes.Observe(static_cast<double>(
      is_binary ? decoded_binary_response.size() : l2_response.size()));

  // Set response content and headers
  if (is_binary) {
    // For binary data, use the raw response (no compression)
    Logger::debug("Proxy sending binary response, content_type={}, size={}",
                  content_type, decoded_binary_response.size());
    res.set_content(decoded_binary_response, content_type);
  } else {
    res.set_content(l2_response, "application/json");
  }

  if (!trace_ctx.m_traceparent_header.empty()) {
    res.set_header("traceparent", trace_ctx.m_traceparent_header);
  }

  const auto span_end_us = get_current_timestamp_us();
  JaegerSpanLogger::log_proxy_response(
      ctx.m_tracer.get(), method, path, status_code, start_us, span_end_us,
      trace_ctx, ctx.m_config.m_mode, request_id);
}
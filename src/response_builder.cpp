#include "response_builder.hpp"
#include "base64_utils.hpp"
#include "common_utils.hpp"
#include "constants.hpp"
#include "app_context.hpp"
#include "error_types.hpp"
#include "json_response_utils.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "nlohmann/json.hpp"
#include "tracing_helpers.hpp"
#include <string>
#include <cstdint>

void set_response_content(httplib::Response &res,
                          const nlohmann::json &parsed_response_data,
                          const std::string &request_id,
                          const TraceContext &trace_ctx,
                          const std::string &method, const std::string &path,
                          uint64_t start_us, AppContext &ctx) {
  if (!parsed_response_data.is_object()) {
    handle_client_error("Invalid response format",
                        &ctx.m_proxy.m_metrics->m_client_errors,
                        http_status::kInternalServerError);
    set_json_error_response(res, http_status::kInternalServerError,
                            "Invalid response format", request_id);
    return;
  }

  // Extract the actual response from l2-server without deep-copying the JSON
  // body string (parsed_response_data outlives the reference below)
  const auto &l2_response = get_body_response_ref(parsed_response_data);
  // safe_get_int: a worker envelope that somehow lacks status_code must produce
  // a 500, never throw (nlohmann operator[] on a const object throws
  // out_of_range, which would escape into the httplib handler thread).
  const int status_code = JsonUtils::safe_get_int(
      parsed_response_data, NatsResponseContract::kStatus,
      http_status::kInternalServerError);
  res.status = status_code;

  // Check if response contains binary data (base64 encoded)
  const bool is_binary =
      get_body_bool(parsed_response_data, NatsResponseContract::kBodyIsBinary);
  const std::string content_type = get_body_string(
      parsed_response_data, NatsResponseContract::kBodyContentType,
      "application/json");

  // For binary data, decode from base64 into a separate buffer
  // Base64 decode using libbase64 library (faster than manual implementation)
  const std::string decoded_binary_response =
      base64::decode_if_binary(l2_response, is_binary);
  if (is_binary) {
    Logger::debug("Proxy decoded binary response, size={}",
                  decoded_binary_response.size());
  }
  const std::string &response_body =
      is_binary ? decoded_binary_response : l2_response;

  increment_and_log_response_sent(ctx.m_proxy.m_metrics->m_bytes_sent, "Proxy",
                                  request_id, status_code);

  ctx.m_proxy.m_metrics->m_response_size_bytes.Observe(
      static_cast<double>(response_body.size()));

  // Set response content and headers
  if (is_binary) {
    // For binary data, use the raw response (no compression)
    Logger::debug("Proxy sending binary response, content_type={}, size={}",
                  content_type, response_body.size());
    res.set_content(response_body, content_type);
  } else {
    // Preserve the upstream Content-Type (the legacy test-port answers
    // text/html on unknown paths); application/json remains the default when
    // the backend sent none.
    res.set_content(response_body, content_type);
  }

  set_traceparent_response_header(res, trace_ctx);

  const auto span_end_us = get_current_timestamp_us();
  JaegerSpanLogger::log_proxy_response(
      ctx.m_tracer.get(), method, path, status_code, start_us, span_end_us,
      trace_ctx, ctx.m_config.m_app.m_mode, request_id);
}
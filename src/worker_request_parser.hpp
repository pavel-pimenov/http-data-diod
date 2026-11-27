#ifndef WORKER_REQUEST_PARSER_HPP
#define WORKER_REQUEST_PARSER_HPP

#include "base64_utils.hpp"
#include "header_utils.hpp"
#include "httplib/httplib.h"
#include "json_utils.hpp"
#include <string>

// Decoded NATS request contract shared between the worker and this parser.
// Kept header-only (no NATS runtime dependency) so the decode logic can be
// unit-tested without a live NATS server.
namespace worker_request_parser {

struct WorkerRequestData {
  std::string m_request_id;
  std::string m_path;
  std::string m_query;
  std::string m_method;
  std::string m_body;
  std::string m_client_ip;
  std::string m_proxy_ip;
  std::string m_traceparent;
  std::string m_proxy_traceparent;
  std::string m_proxy_span_id;
  httplib::Headers m_forwarded_headers;
};

[[nodiscard]] inline WorkerRequestData decode_nats_request(const json &request_data) {
  WorkerRequestData metadata;

  // Use explicit .get<std::string>() for clarity and potential move
  // optimization
  metadata.m_request_id =
      request_data[NatsContract::kRequestId].get<std::string>();
  metadata.m_path = request_data[NatsContract::kPath].get<std::string>();
  metadata.m_query =
      JsonUtils::get_string_or(request_data, NatsContract::kQuery);
  metadata.m_method = request_data[NatsContract::kMethod].get<std::string>();

  // JSON bodies are carried verbatim; non-JSON bodies are base64 in the
  // envelope (see prepare_request_data) and must be decoded back to bytes
  // before the upstream HTTP call.
  const std::string body = request_data[NatsContract::kBody].get<std::string>();
  metadata.m_body = base64::decode_if_binary(
      body,
      JsonUtils::get_bool_or(request_data, NatsContract::kBodyIsBinary));

  // Extract optional fields with defaults
  metadata.m_client_ip = JsonUtils::get_string_or(
      request_data, NatsContract::kClientIp, "unknown");
  metadata.m_proxy_ip = JsonUtils::get_string_or(
      request_data, NatsContract::kProxyIp, "unknown");
  metadata.m_proxy_traceparent =
      JsonUtils::get_string_or(request_data, NatsContract::kProxyTraceparent);
  metadata.m_proxy_span_id =
      JsonUtils::get_string_or(request_data, NatsContract::kProxySpanId);

  // Extract traceparent if present
  metadata.m_traceparent =
      JsonUtils::get_string_or(request_data, NatsContract::kTraceparent);

  if (request_data.contains(NatsContract::kHeaders)) {
    HeaderUtils::filter_headers_from_json(
        request_data[NatsContract::kHeaders], metadata.m_forwarded_headers,
        HeaderUtils::get_default_skip_headers(), "Worker");
  }

  return metadata;
}

} // namespace worker_request_parser

#endif
#ifndef WORKER_REQUEST_PARSER_HPP
#define WORKER_REQUEST_PARSER_HPP

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
  metadata.m_query = request_data.value(NatsContract::kQuery, std::string{});
  metadata.m_method = request_data[NatsContract::kMethod].get<std::string>();

  // Extract body as-is
  metadata.m_body = request_data[NatsContract::kBody].get<std::string>();

  // Extract optional fields with defaults
  metadata.m_client_ip =
      request_data.value(NatsContract::kClientIp, std::string("unknown"));
  metadata.m_proxy_ip =
      request_data.value(NatsContract::kProxyIp, std::string("unknown"));
  metadata.m_proxy_traceparent =
      request_data.value(NatsContract::kProxyTraceparent, std::string{});
  metadata.m_proxy_span_id =
      request_data.value(NatsContract::kProxySpanId, std::string{});

  // Extract traceparent if present
  metadata.m_traceparent =
      request_data.value(NatsContract::kTraceparent, std::string{});

  if (request_data.contains(NatsContract::kHeaders)) {
    HeaderUtils::filter_headers_from_json(
        request_data[NatsContract::kHeaders], metadata.m_forwarded_headers,
        HeaderUtils::get_default_skip_headers(), "Worker");
  }

  return metadata;
}

} // namespace worker_request_parser

#endif
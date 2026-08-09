#include "request_data_preparer.hpp"
#include "common_utils.hpp"
#include "header_utils.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include <algorithm>

nlohmann::json
prepare_request_data(const std::string &request_id, const std::string &method,
                     const std::string &path, const std::string &body,
                     const httplib::Request &req,
                     const std::string &traceparent_for_backend) {
  // Prepare request data for backend with pre-allocated capacity
  nlohmann::json request_data = nlohmann::json::object();
  request_data[NatsContract::kRequestId] = request_id;
  request_data[NatsContract::kMethod] = method;
  request_data[NatsContract::kPath] = path;
  request_data[NatsContract::kQuery] = extract_query_string(req);
  request_data[NatsContract::kTraceparent] = traceparent_for_backend;

  // Extract and include client IP (from X-Forwarded-For/X-Real-IP or direct)
  request_data[NatsContract::kClientIp] = extract_client_ip(req);
  request_data[NatsContract::kProxyIp] = extract_proxy_ip(req);

  // Include request body as-is (compression was removed)
  request_data[NatsContract::kBody] = body;

  // Include client headers, filtering out some
  nlohmann::json headers_json = nlohmann::json::object();
  HeaderUtils::filter_headers_to_json(req.headers, headers_json,
                                      HeaderUtils::get_default_skip_headers(),
                                      "Proxy");

  if (!headers_json.empty()) {
    Logger::debug("Proxy forwarding {} client headers to backend",
                  headers_json.size());
    request_data[NatsContract::kHeaders] = std::move(headers_json);
  }

  return request_data;
}
#ifndef JSON_RESPONSE_UTILS_HPP
#define JSON_RESPONSE_UTILS_HPP

#include "httplib/httplib.h"
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

// HTTP JSON-response helpers shared by the request handlers and the health
// endpoints. Hosted here so the umbrella common_utils header re-exports them
// without embedding the implementations.

// Writes a JSON error body with an optional request_id.
inline void set_json_error_response(httplib::Response &res, int status,
                                    std::string_view message,
                                    std::string_view request_id = "") {
  res.status = status;
  nlohmann::json body;
  body["error"] = message;
  if (!request_id.empty()) {
    body["request_id"] = request_id;
  }
  res.set_content(body.dump(), "application/json");
}

// Serializes a JSON body and sets the application/json content type. Replaces
// the repeated `res.status = s; res.set_content(body.dump(), "application/json")`
// idiom so the status+content-type pairing lives in one place.
inline void send_json_response(httplib::Response &res, int status,
                               const nlohmann::json &body) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

inline void set_health_alive(httplib::Response &res,
                             std::string_view service) {
  res.status = 200;
  res.set_content(
      std::format(R"({{"status": "alive", "service": "{}"}})", service),
      "application/json");
}

inline void set_health_ready(httplib::Response &res,
                             std::string_view service) {
  res.status = 200;
  res.set_content(
      std::format(R"({{"status": "ready", "service": "{}"}})", service),
      "application/json");
}

#endif // JSON_RESPONSE_UTILS_HPP

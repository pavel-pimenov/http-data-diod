#ifndef JSON_RESPONSE_UTILS_HPP
#define JSON_RESPONSE_UTILS_HPP

#include "constants.hpp"
#include "httplib/httplib.h"
#include <format>
#include <nlohmann/json.hpp>
#include <prometheus/gauge.h>
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
  res.status = http_status::kOk;
  res.set_content(
      std::format(R"({{"status": "alive", "service": "{}"}})", service),
      "application/json");
}

inline void set_health_ready(httplib::Response &res,
                             std::string_view service) {
  res.status = http_status::kOk;
  res.set_content(
      std::format(R"({{"status": "ready", "service": "{}"}})", service),
      "application/json");
}

// Readiness body for the NATS-backed services (worker/proxy): the same shape as
// set_health_ready plus the "messaging": "nats" marker. Both readiness
// endpoints used to inline the identical JSON template.
inline void set_health_ready_nats(httplib::Response &res,
                                  std::string_view service) {
  res.status = http_status::kOk;
  res.set_content(
      std::format(
          R"({{"status": "ready", "service": "{}", "messaging": "nats"}})",
          service),
      "application/json");
}

// Readiness failure body for the NATS-backed services: 503
// (kServiceUnavailable) plus the same shape
// as set_health_ready_nats with "error" instead of "messaging". The worker used
// a raw string literal and the proxy an nlohmann object for the very same body.
inline void set_health_not_ready_nats(httplib::Response &res,
                                      std::string_view service,
                                      std::string_view error) {
  res.status = http_status::kServiceUnavailable;
  res.set_content(
      std::format(
          R"({{"status": "not_ready", "service": "{}", "error": "{}"}})",
          service, error),
      "application/json");
}

// Mirrors NATS connectivity into the two gauges the /health/ready endpoint
// updates: the connection state read by the availability panels and the
// readiness flag itself. Both NATS-backed handlers (worker, proxy) set them
// with the same ternary pair.
inline void set_nats_readiness_gauges(prometheus::Gauge &nats_connected,
                                      prometheus::Gauge &health_ready,
                                      bool ready) {
  const double value = ready ? 1.0 : 0.0;
  nats_connected.Set(value);
  health_ready.Set(value);
}

// Mirrors the NATS connection state into the readiness gauges and writes the
// readiness answer. The proxy and the worker answer /health/ready the same way
// — only the readiness check and the service name differ — so the pair of calls
// lives here instead of being copied into both handlers.
inline void write_nats_readiness(httplib::Response &res,
                                 std::string_view service,
                                 prometheus::Gauge &nats_connected,
                                 prometheus::Gauge &health_ready, bool ready,
                                 std::string_view error) {
  set_nats_readiness_gauges(nats_connected, health_ready, ready);
  if (ready) {
    set_health_ready_nats(res, service);
  } else {
    set_health_not_ready_nats(res, service, error);
  }
}

#endif // JSON_RESPONSE_UTILS_HPP

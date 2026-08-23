#ifndef COMMON_UTILS_HPP
#define COMMON_UTILS_HPP

// Umbrella header — includes all sub-modules for backward compatibility.
// New code should include specific sub-headers directly.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "base64_utils.hpp"
#include "error_types.hpp"
#include "pool_executor.hpp"
#include "url_utils.hpp"

#include "httplib/httplib.h"
#include "logger.hpp"
#include "nlohmann/json.hpp"
#include "scoped_metrics.hpp"
#include "scoped_profiler.hpp"
#include "time_utils.hpp"
#include "trace_logger.hpp"
#include <prometheus/counter.h>
#include <prometheus/gauge.h>

// Forward declarations to avoid circular dependencies
class JaegerLogger;

using json = nlohmann::json;

inline std::string read_request_body(const httplib::Request &req) {
  return req.body;
}

inline std::string
get_header_value(const httplib::Headers &headers, const std::string &name,
                 const std::string &default_value = "unknown") {
  const auto it = headers.find(name);
  if (it != headers.end() && !it->second.empty()) {
    return it->second;
  }
  return default_value;
}

inline std::string shorten_user_agent(const std::string &ua) {
  constexpr size_t max_len = 80;
  if (ua.size() <= max_len) {
    return ua;
  }

  // Extract browser name+version from long UA strings
  struct BrowserPattern {
    const char *m_marker;
    const char *m_name;
  };
  // Order matters: check Edg (Edge) before generic "e" patterns
  constexpr BrowserPattern patterns[] = {
      {"Edg/", "Edge/"},    {"Chrome/", "Chrome/"}, {"Firefox/", "Firefox/"},
      {"Opera/", "Opera/"}, {"OPR/", "Opera/"},     {"Version/", "Safari/"},
  };

  for (const auto &p : patterns) {
    const auto pos = ua.find(p.m_marker);
    if (pos == std::string::npos) {
      continue;
    }
    const auto start = pos;
    auto end = ua.find(' ', start);
    if (end == std::string::npos) {
      end = ua.size();
    }
    return std::string(p.m_name) +
           std::string(ua.begin() + start + std::strlen(p.m_marker),
                       ua.begin() + end);
  }

  // Not a recognized browser or no pattern found — truncate
  return ua.substr(0, max_len - 3) + "...";
}

inline void set_json_error_response(httplib::Response &res, int status,
                                     const std::string &message,
                                     const std::string &request_id = "") {
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

// Logs the error, increments the prometheus counter (if set), writes a JSON
// error response and returns false — the standard failure-exit idiom for
// request handlers. log_message defaults to message, allowing a detailed log
// line to differ from the response body text.
inline bool fail_request(httplib::Response &res, int status,
                         const std::string &message,
                         prometheus::Counter *counter,
                         const std::string &request_id = "",
                         const std::string &log_message = "") {
  handle_error(log_message.empty() ? message : log_message, counter, true);
  set_json_error_response(res, status, message, request_id);
  return false;
}

inline std::string resolve_parent_id(const std::string &parent_span_id,
                                     const std::string &fallback_parent_id) {
  return parent_span_id.empty() ? fallback_parent_id : parent_span_id;
}

inline void set_health_alive(httplib::Response &res,
                             const std::string &service) {
  res.status = 200;
  res.set_content(R"({"status": "alive", "service": ")" + service + R"("})",
                  "application/json");
}

inline void set_health_ready(httplib::Response &res,
                             const std::string &service) {
  res.status = 200;
  res.set_content(R"({"status": "ready", "service": ")" + service + R"("})",
                  "application/json");
}

std::expected<json, std::string> parse_json(const std::string &body);

// Safe log preview for request/response bodies: returns the full text for
// small payloads, otherwise truncates to max_len with a byte-count suffix.
// Use this instead of logging raw bodies to keep logs compact and leak-free.
std::string log_body_preview(const std::string &body, size_t max_len = 512);

// SHA-256 hex digest of a byte string (OpenSSL). Used to key per-body
// duplicate detection in the proxy and the correlation-test req_hash echo.
std::string compute_sha256_hex(const std::string &data);

struct TraceContext {
  std::string m_trace_id;
  std::string m_span_id;
  std::string m_parent_id;
  std::string m_traceparent_header;
  bool m_sampled = false;
};

TraceContext handle_trace_context(const std::string &traceparent_raw,
                                  JaegerLogger *tracer);

inline uint64_t get_current_timestamp_us() {
  return static_cast<uint64_t>(TimeUtils::epoch_us());
}

void log_span_to_jaeger(JaegerLogger *tracer, const std::string &method,
                        const std::string &url, int status_code,
                        uint64_t start_us, uint64_t end_us,
                        const std::string &service_name,
                        const std::string &request_id = "",
                        const std::string &trace_id = "",
                        const std::string &span_id = "",
                        const std::string &parent_id = "",
                        const nlohmann::json &additional_attributes = {});

std::expected<json, std::string>
validate_and_parse_json(const std::string &body,
                        const std::string &context = "",
                        const std::string &request_id = "");

void log_request_received(const std::string &context, size_t body_size);
void log_response_sent(const std::string &context,
                       const std::string &request_id, int status_code);
void increment_and_log_request_received(prometheus::Counter &metrics_counter,
                                        const std::string &context,
                                        size_t body_size);
void increment_and_log_response_sent(prometheus::Counter &metrics_counter,
                                     const std::string &context,
                                     const std::string &request_id,
                                     int status_code);

inline ScopedMetrics
create_scoped_request_metrics(prometheus::Counter &metrics_counter) {
  return ScopedMetrics(metrics_counter);
}

inline ScopedProfiler
create_scoped_request_profiler(prometheus::Histogram &histogram) {
  return ScopedProfiler(histogram);
}

// RAII guard bundling the standard request-handler prologue: the duration
// histogram profiler, the request counter and the start timestamp. Members are
// destroyed at scope end in the same order the separate locals were, so the
// profiler/counter fire exactly as before. start_us() is read-only after
// construction.
class RequestScopedTiming {
public:
  RequestScopedTiming(prometheus::Histogram &histogram,
                      prometheus::Counter &counter)
      : m_profiler(histogram), m_counter(counter),
        m_start_us(get_current_timestamp_us()) {}

  uint64_t start_us() const { return m_start_us; }

private:
  ScopedProfiler m_profiler;
  ScopedMetrics m_counter;
  const uint64_t m_start_us;
};

std::string format_http_error(httplib::Error error, int timeout_seconds,
                              const std::string &operation);

void setup_ssl_client(httplib::SSLClient &client, int timeout_seconds,
                      bool enable_cert_verification,
                      bool enable_hostname_verification,
                      const std::string &ca_cert_path,
                      bool enable_connection_reuse);

void validate_trace_context(const TraceContext &ctx,
                            const std::string &context);

template <typename T>
inline bool validate_range(const T &value, const std::string &name, T min,
                           T max, T warn_threshold = 0) {
  if (value <= min || value > max) {
    Logger::error("Invalid {}: {} (must be {}-{})", name, value, min, max);
    return false;
  }
  if (warn_threshold > 0 && value > warn_threshold) {
    Logger::warn("Very high {}: {} (recommended: < {})", name, value,
                 warn_threshold);
  }
  return true;
}

template <typename T>
inline bool validate_positive(const T &value, const std::string &name) {
  if (value <= 0) {
    Logger::error("Invalid {}: {} (must be > 0)", name, value);
    return false;
  }
  return true;
}

class RetryHandler {
public:
  explicit RetryHandler(int initial_delay_ms = 100, int max_delay_ms = 2000)
      : m_initial_delay_ms(initial_delay_ms), m_max_delay_ms(max_delay_ms),
        m_current_delay_ms(initial_delay_ms), m_consecutive_failures(0) {}

  void record_failure() {
    m_consecutive_failures++;
    m_current_delay_ms = std::min(m_current_delay_ms * 2, m_max_delay_ms);
  }

  void record_success() {
    m_consecutive_failures = 0;
    m_current_delay_ms = m_initial_delay_ms;
  }

  int get_consecutive_failures() const { return m_consecutive_failures; }
  int get_current_delay_ms() const { return m_current_delay_ms; }

private:
  int m_initial_delay_ms;
  int m_max_delay_ms;
  int m_current_delay_ms;
  int m_consecutive_failures = 0;
};

#endif // COMMON_UTILS_HPP

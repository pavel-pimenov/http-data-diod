#ifndef JSON_UTILS_HPP
#define JSON_UTILS_HPP

#include <expected>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

using json = nlohmann::json;

// JSON-contract key names shared between the proxy and the worker (NATS mode).
// Centralising them means a rename in one participant can no longer silently
// break the other.
namespace NatsContract {
inline constexpr const char *kRequestId = "request_id";
inline constexpr const char *kMethod = "method";
inline constexpr const char *kPath = "path";
inline constexpr const char *kQuery = "query";
inline constexpr const char *kBody = "body";
inline constexpr const char *kClientIp = "client_ip";
inline constexpr const char *kProxyIp = "proxy_ip";
inline constexpr const char *kTraceparent = "traceparent";
inline constexpr const char *kHeaders = "headers";
inline constexpr const char *kProxySpanId = "proxy_span_id";
inline constexpr const char *kProxyInletSpanId = "proxy_inlet_span_id";
inline constexpr const char *kProxyTraceId = "proxy_trace_id";
inline constexpr const char *kProxyTraceparent = "proxy_traceparent";
inline constexpr const char *kTimestamp = "timestamp";
// Custom NATS header linking the poll and consume spans.
inline constexpr const char *kConsumeSpanIdHeader = "X-Consume-Span-Id";
} // namespace NatsContract

// JSON keys of the worker->proxy response contract (write in l2_worker_nats,
// read in response_builder).
namespace NatsResponseContract {
inline constexpr const char *kStatus = "status_code";
inline constexpr const char *kHeaders = "headers";
inline constexpr const char *kBody = "body";
inline constexpr const char *kBodyRequestId = "request_id";
inline constexpr const char *kBodyResponse = "response";
inline constexpr const char *kBodyTimestamp = "timestamp";
inline constexpr const char *kBodyIsBinary = "is_binary";
inline constexpr const char *kBodyContentType = "content_type";
inline constexpr const char *kBodyTraceparent = "traceparent";
} // namespace NatsResponseContract

class JsonUtils {
public:
  static std::expected<json, std::string> try_parse(const std::string &s) {
    try {
      return json::parse(s);
    } catch (const json::parse_error &e) {
      return std::unexpected(std::string(e.what()));
    }
  }

  static std::string safe_get_string(const json &j, const std::string &key,
                                     const std::string &fallback = "") {
    const auto it = j.find(key);
    if (it != j.end() && it->is_string()) {
      return it->get<std::string>();
    }
    return fallback;
  }

  static int safe_get_int(const json &j, const std::string &key,
                          int fallback = 0) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number()) {
      return it->get<int>();
    }
    return fallback;
  }

  static bool safe_get_bool(const json &j, const std::string &key,
                            bool fallback = false) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) {
      return it->get<bool>();
    }
    return fallback;
  }

  static bool has_key(const json &j, const std::string &key) {
    return j.contains(key) && !j[key].is_null();
  }

  static bool is_array(const json &j) { return j.is_array(); }
};

#endif
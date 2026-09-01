#ifndef JSON_UTILS_HPP
#define JSON_UTILS_HPP

#include <cstdint>
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

// JSON keys of the proxy<->worker contract of the HTTP DB Gateway. A query
// request is published by the proxy on the DB subject; the worker executes it
// against a database and replies with a NatsResponseContract-style envelope
// carrying the DB gateway JSON body (kDbStatus/kDbBody).
namespace DbQueryContract {
inline constexpr const char *kType = "type";
inline constexpr const char *kTypeQuery = "query";
inline constexpr const char *kTypePing = "ping";
inline constexpr const char *kRequestId = "request_id";
inline constexpr const char *kDb = "db";
inline constexpr const char *kSql = "sql";
inline constexpr const char *kParams = "params";
inline constexpr const char *kTimeoutMs = "timeout_ms";
inline constexpr const char *kMaxRows = "max_rows";
// Worker->proxy envelope: kStatus is the HTTP status of the DB gateway
// response, kBody is its JSON body (or an ErrorResponse object).
inline constexpr const char *kStatus = "status";
inline constexpr const char *kBody = "body";
} // namespace DbQueryContract

// JSON keys of the DB Gateway HTTP response body (documented in
// docs/openapi/http-db-gate.yaml).
namespace DbResponseContract {
inline constexpr const char *kStatus = "status";
inline constexpr const char *kStatusOk = "ok";
inline constexpr const char *kStatusError = "error";
inline constexpr const char *kDb = "db";
inline constexpr const char *kLatencyMs = "latency_ms";
inline constexpr const char *kColumns = "columns";
inline constexpr const char *kName = "name";
inline constexpr const char *kType = "type";
inline constexpr const char *kRows = "rows";
inline constexpr const char *kRowCount = "row_count";
inline constexpr const char *kTruncated = "truncated";
inline constexpr const char *kDurationMs = "duration_ms";
inline constexpr const char *kError = "error";
inline constexpr const char *kCode = "code";
inline constexpr const char *kMessage = "message";
inline constexpr const char *kDetail = "detail";
} // namespace DbResponseContract

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

// Builds the worker->proxy NATS response envelope (the JSON contract in
// NatsResponseContract). Centralises the 30-line JSON assembly from the
// l2_worker response path so the shape stays in one, testable place.
inline json build_nats_response_envelope(
    int status_code, const std::string &request_id,
    const std::string &response, uint64_t timestamp_us, bool is_binary,
    const std::string &content_type, const json &headers_json,
    const std::string &traceparent_header) {
  json envelope;
  envelope[NatsResponseContract::kStatus] = status_code;
  if (!headers_json.empty()) {
    envelope[NatsResponseContract::kHeaders] = headers_json;
  }
  json &body = envelope[NatsResponseContract::kBody];
  body[NatsResponseContract::kBodyRequestId] = request_id;
  body[NatsResponseContract::kBodyResponse] = response;
  body[NatsResponseContract::kBodyTimestamp] = timestamp_us;
  body[NatsResponseContract::kBodyIsBinary] = nlohmann::json(is_binary);
  body[NatsResponseContract::kBodyContentType] = content_type;
  if (!traceparent_header.empty()) {
    body[NatsResponseContract::kBodyTraceparent] = traceparent_header;
  }
  return envelope;
}

#endif
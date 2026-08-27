#include "common_utils.hpp"
#include "base64_utils.hpp"
#include "error_types.hpp"
#include "header_utils.hpp"
#include "json_utils.hpp"
#include "url_utils.hpp"
#include <array>
#include <format>
#include <iomanip>
#include <openssl/sha.h>
#include <span>
#include <sstream>
#include <string_view>
#if __has_include(<print>)
#include <print>
#endif

std::string compute_sha256_hex(std::string_view data) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), hash);
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (const unsigned char byte : hash) oss << std::setw(2) << static_cast<int>(byte);
#if __has_include(<print>) && defined(__cpp_lib_print)
  // std::print demo — fire-and-forget debug (no-op в prod, guarded)
  // std::println("sha256 {} bytes -> {}...", data.size(), oss.str().substr(0,8));
  (void)data;
#endif
  return oss.str();
}

std::string log_body_preview(std::string_view body, size_t max_len) {
  if (body.size() <= max_len) {
    return std::string(body);
  }
  return std::format("{}... ({} bytes total)", body.substr(0, max_len),
                     body.size());
}

std::expected<json, std::string> parse_json(std::string_view body) {
  const auto result = JsonUtils::try_parse(std::string(body));
  if (result) {
    return result;
  }
  auto body_preview = std::string(body);
  if (body_preview.length() > 100) {
    body_preview = std::string(body.substr(0, 100)) + "...";
  }
  return std::unexpected(
      std::format("Failed to parse JSON: {}\nBody preview: {}", result.error(),
                  body_preview));
}

TraceContext handle_trace_context(std::string_view traceparent_raw,
                                  JaegerLogger *tracer) {
  TraceContext ctx;
  ctx.m_sampled = true;

  if (!traceparent_raw.empty() && tracer &&
      tracer->parse_traceparent(traceparent_raw, ctx.m_trace_id,
                                ctx.m_parent_id, ctx.m_sampled)) {
    ctx.m_span_id = tracer->generate_span_id();
  } else if (tracer) {
    ctx.m_trace_id = tracer->generate_trace_id();
    ctx.m_span_id = tracer->generate_span_id();
    ctx.m_parent_id = "";
  } else {
    ctx.m_trace_id = "";
    ctx.m_span_id = "";
    ctx.m_parent_id = "";
    ctx.m_traceparent_header = "";
    return ctx;
  }

  // Both traced branches converge here: rebuild the traceparent from the
  // freshly generated span id so the downstream span links to the log records.
  ctx.m_traceparent_header = tracer->generate_traceparent(
      ctx.m_trace_id, ctx.m_span_id, ctx.m_sampled);

  return ctx;
}

void handle_error(const std::string &error_msg,
                  prometheus::Counter *metrics_counter, bool log_error) {
  if (log_error) {
    Logger::error(error_msg);
  } else {
    Logger::warn(error_msg);
  }

  if (metrics_counter) {
    metrics_counter->Increment();
  }
}

void handle_exception(const std::exception &e,
                      prometheus::Counter *metrics_counter,
                      const std::string &prefix_msg) {
  const auto error_msg =
      prefix_msg.empty() ? std::string(e.what())
                         : std::format("{}: {}", prefix_msg, e.what());
  handle_error(error_msg, metrics_counter, true);
}

void handle_http_error(const std::string &error_msg,
                       prometheus::Counter *metrics_counter,
                       const std::string &operation, int attempt,
                       const std::string &url) {
  std::string full_error_msg;
  if (!url.empty() && attempt > 0) {
    full_error_msg = std::format("HTTP {} error for URL: {} (attempt {}): {}",
                                 operation, url, attempt, error_msg);
  } else if (!url.empty()) {
    full_error_msg =
        std::format("HTTP {} error for URL: {}: {}", operation, url, error_msg);
  } else if (attempt > 0) {
    full_error_msg = std::format("HTTP {} error (attempt {}): {}", operation,
                                 attempt, error_msg);
  } else {
    full_error_msg = std::format("HTTP {} error: {}", operation, error_msg);
  }

  Logger::error(full_error_msg);

  if (metrics_counter) {
    metrics_counter->Increment();
  }
}

void log_span_to_jaeger(JaegerLogger *tracer, const std::string &method,
                        const std::string &url, int status_code,
                        uint64_t start_us, uint64_t end_us,
                        const std::string &service_name,
                        const std::string &request_id,
                        const std::string &trace_id, const std::string &span_id,
                        const std::string &parent_id,
                        const nlohmann::json &additional_attributes) {
  if (tracer) {
    tracer->log_request(method, url, status_code, start_us, end_us,
                        service_name, request_id, trace_id, span_id, parent_id,
                        additional_attributes);
  }
}

namespace {
// Maps an enum value to its string name via an array of names indexed by the
// underlying enum value. Unknown values fall back to "UNKNOWN".
template <typename Enum, size_t N>
std::string_view enum_to_string(Enum value,
                                const std::array<const char *, N> &names) {
  const auto idx = static_cast<size_t>(value);
  if (idx < N && names[idx] != nullptr) {
    return names[idx];
  }
  return "UNKNOWN";
}

// Rule of the keyword-based error categorizers: matches when any of
// m_keywords occurs in the lowercased error message.
template <typename Enum> struct ErrorCategoryRule {
  Enum m_type;
  std::span<const std::string_view> m_keywords;
};

// Walks the rules in order (first match wins, mirroring the historic
// if/else-if chains) and returns the matching category, or Enum::OTHER_ERROR.
template <typename Enum, size_t N>
Enum categorize_by_keywords(
    const std::string &lower_msg,
    const std::array<ErrorCategoryRule<Enum>, N> &rules) {
  for (const auto &rule : rules) {
    for (const std::string_view keyword : rule.m_keywords) {
      if (lower_msg.find(keyword) != std::string::npos) {
        return rule.m_type;
      }
    }
  }
  return Enum::OTHER_ERROR;
}

// Logs an error with its category prefix, bumps the total counter and the
// counter of the specific category (or other_counter when the category has no
// dedicated counter).
template <typename Enum, size_t N>
void handle_error_with_category(
    const std::string &error_msg, Enum error_type, std::string_view type_str,
    const std::string &prefix, prometheus::Counter *total_counter,
    prometheus::Counter *other_counter,
    const std::array<std::pair<Enum, prometheus::Counter *>, N>
        &specific_counters) {
  Logger::error("{} [{}]: {}", prefix, type_str, error_msg);
  if (total_counter != nullptr) {
    total_counter->Increment();
  }
  for (const auto &[type, counter] : specific_counters) {
    if (type == error_type) {
      if (counter != nullptr) {
        counter->Increment();
      }
      return;
    }
  }
  if (other_counter != nullptr) {
    other_counter->Increment();
  }
}

// Keyword tables for the error categorizers. Rule order matters: the first
// rule whose keyword matches wins (same semantics as the previous if/else-if
// chains).
constexpr std::array<std::string_view, 3> g_http_connection_keywords = {
    "connection", "could not resolve", "dns"};
constexpr std::array<std::string_view, 4> g_http_ssl_keywords = {
    "ssl", "tls", "certificate", "handshake"};
constexpr std::array<std::string_view, 2> g_http_timeout_keywords = {
    "timeout", "timed out"};
constexpr std::array<std::string_view, 2> g_http_protocol_keywords = {
    "protocol", "invalid"};
constexpr std::array<std::string_view, 2> g_l2_connection_keywords = {
    "connection", "could not resolve"};
constexpr std::array<std::string_view, 2> g_l2_timeout_keywords = {"timeout",
                                                                   "timed out"};
constexpr std::array<std::string_view, 2> g_l2_retry_keywords = {"retry",
                                                                 "attempts"};
constexpr std::array<std::string_view, 3> g_l2_response_keywords = {
    "json", "parse", "invalid"};
constexpr std::array<std::string_view, 2> g_processing_json_keywords = {
    "json", "parse"};
constexpr std::array<std::string_view, 3> g_processing_validation_keywords = {
    "valid", "required", "missing"};
constexpr std::array<std::string_view, 3> g_processing_decompression_keywords =
    {"gzip", "decompress", "inflate"};
constexpr std::array<std::string_view, 3> g_processing_encoding_keywords = {
    "base64", "encode", "decode"};
constexpr std::array<std::string_view, 1> g_processing_timeout_keywords = {
    "timeout"};
constexpr std::array<std::string_view, 2> g_processing_resource_keywords = {
    "exhausted", "no available"};

constexpr std::array g_http_keyword_rules = {
    ErrorCategoryRule<HttpErrorType>{HttpErrorType::CONNECTION_ERROR,
                                     g_http_connection_keywords},
    ErrorCategoryRule<HttpErrorType>{HttpErrorType::SSL_ERROR,
                                     g_http_ssl_keywords},
    ErrorCategoryRule<HttpErrorType>{HttpErrorType::TIMEOUT_ERROR,
                                     g_http_timeout_keywords},
    ErrorCategoryRule<HttpErrorType>{HttpErrorType::PROTOCOL_ERROR,
                                     g_http_protocol_keywords}};
constexpr std::array g_l2_keyword_rules = {
    ErrorCategoryRule<L2ErrorType>{L2ErrorType::CONNECTION_ERROR,
                                   g_l2_connection_keywords},
    ErrorCategoryRule<L2ErrorType>{L2ErrorType::TIMEOUT_ERROR,
                                   g_l2_timeout_keywords},
    ErrorCategoryRule<L2ErrorType>{L2ErrorType::RETRY_EXHAUSTED,
                                   g_l2_retry_keywords},
    ErrorCategoryRule<L2ErrorType>{L2ErrorType::RESPONSE_ERROR,
                                   g_l2_response_keywords}};
constexpr std::array g_processing_keyword_rules = {
    ErrorCategoryRule<ProcessingErrorType>{
        ProcessingErrorType::JSON_PARSE_ERROR, g_processing_json_keywords},
    ErrorCategoryRule<ProcessingErrorType>{
        ProcessingErrorType::VALIDATION_ERROR,
        g_processing_validation_keywords},
    ErrorCategoryRule<ProcessingErrorType>{
        ProcessingErrorType::DECOMPRESSION_ERROR,
        g_processing_decompression_keywords},
    ErrorCategoryRule<ProcessingErrorType>{ProcessingErrorType::ENCODING_ERROR,
                                           g_processing_encoding_keywords},
    ErrorCategoryRule<ProcessingErrorType>{ProcessingErrorType::TIMEOUT_ERROR,
                                           g_processing_timeout_keywords},
    ErrorCategoryRule<ProcessingErrorType>{
        ProcessingErrorType::RESOURCE_EXHAUSTED,
        g_processing_resource_keywords}};

constexpr std::array<const char *, 9> g_http_error_names = {
    "CONNECTION_ERROR",  "SSL_ERROR",      "TIMEOUT_ERROR",
    "STATUS_CODE_ERROR", "PROTOCOL_ERROR", "RATE_LIMIT_ERROR",
    "SERVER_ERROR",      "CLIENT_ERROR",   "OTHER_ERROR"};
constexpr std::array<const char *, 5> g_l2_error_names = {
    "CONNECTION_ERROR", "TIMEOUT_ERROR", "RESPONSE_ERROR", "RETRY_EXHAUSTED",
    "OTHER_ERROR"};
constexpr std::array<const char *, 7> g_processing_error_names = {
    "JSON_PARSE_ERROR", "VALIDATION_ERROR", "DECOMPRESSION_ERROR",
    "ENCODING_ERROR",   "TIMEOUT_ERROR",    "RESOURCE_EXHAUSTED",
    "OTHER_ERROR"};
} // namespace

HttpErrorType categorize_http_error(const std::string &error_msg,
                                    int status_code) {
  if (status_code >= 500) {
    return HttpErrorType::SERVER_ERROR;
  }
  if (status_code == 429) {
    return HttpErrorType::RATE_LIMIT_ERROR;
  }
  if (status_code >= 400) {
    return HttpErrorType::CLIENT_ERROR;
  }
  return categorize_by_keywords(HeaderUtils::to_lower(error_msg),
                                g_http_keyword_rules);
}

L2ErrorType categorize_l2_error(const std::string &error_msg) {
  return categorize_by_keywords(HeaderUtils::to_lower(error_msg),
                                g_l2_keyword_rules);
}

ProcessingErrorType categorize_processing_error(const std::string &error_msg) {
  return categorize_by_keywords(HeaderUtils::to_lower(error_msg),
                                g_processing_keyword_rules);
}

std::string http_error_type_to_string(HttpErrorType type) {
  return std::string(enum_to_string(type, g_http_error_names));
}

std::string l2_error_type_to_string(L2ErrorType type) {
  return std::string(enum_to_string(type, g_l2_error_names));
}

std::string processing_error_type_to_string(ProcessingErrorType type) {
  return std::string(enum_to_string(type, g_processing_error_names));
}

void handle_l2_error_with_category(const std::string &error_msg,
                                   const L2ErrorMetrics &metrics,
                                   const std::string &operation) {
  const L2ErrorType error_type = categorize_l2_error(error_msg);
  handle_error_with_category(
      error_msg, error_type, l2_error_type_to_string(error_type),
      "L2 " + operation + " error", metrics.m_total_errors,
      metrics.m_other_errors,
      std::array{
          std::pair{L2ErrorType::CONNECTION_ERROR, metrics.m_connection_errors},
          std::pair{L2ErrorType::TIMEOUT_ERROR, metrics.m_timeout_errors}});
}

void handle_processing_error_with_category(
    const std::string &error_msg, const ProcessingErrorMetrics &metrics,
    const std::string &operation) {
  const ProcessingErrorType error_type = categorize_processing_error(error_msg);
  handle_error_with_category(
      error_msg, error_type, processing_error_type_to_string(error_type),
      "Processing " + operation + " error", metrics.m_total_errors,
      metrics.m_other_errors,
      std::array{std::pair{ProcessingErrorType::JSON_PARSE_ERROR,
                           metrics.m_json_errors},
                 std::pair{ProcessingErrorType::VALIDATION_ERROR,
                           metrics.m_validation_errors},
                 std::pair{ProcessingErrorType::DECOMPRESSION_ERROR,
                           metrics.m_decompression_errors}});
}

std::expected<json, std::string>
validate_and_parse_json(std::string_view body, std::string_view context,
                        std::string_view request_id) {
  const auto result = parse_json(body);
  if (!result) {
    const auto log_msg = std::format(
        "{} failed to parse JSON{}: {}", context,
        request_id.empty() ? ""
                           : std::format(" for request_id: {}", request_id),
        result.error());
    Logger::error(log_msg);
    return std::unexpected(result.error());
  }
  return result;
}

void log_request_received(const std::string &context, size_t body_size) {
  Logger::debug("{} received request, body size: {}", context, body_size);
}

void log_response_sent(const std::string &context,
                       const std::string &request_id, int status_code) {
  if (request_id.empty()) {
    Logger::debug("{} sending response, status: {}", context, status_code);
  } else {
    Logger::debug("{} sending response for request_id: {}, status: {}", context,
                  request_id, status_code);
  }
}

void increment_and_log_request_received(prometheus::Counter &metrics_counter,
                                        const std::string &context,
                                        size_t body_size) {
  metrics_counter.Increment();
  log_request_received(context, body_size);
}

void increment_and_log_response_sent(prometheus::Counter &metrics_counter,
                                     const std::string &context,
                                     const std::string &request_id,
                                     int status_code) {
  metrics_counter.Increment();
  log_response_sent(context, request_id, status_code);
}

ParsedUrl parse_url(std::string_view url) {
  ParsedUrl result;
  result.m_host.clear();
  result.m_path = "/";
  result.m_port = 80;

  if (url.empty()) {
    throw std::runtime_error("Invalid URL: empty string");
  }

  const size_t protocol_end = url.find("://");
  if (protocol_end != std::string_view::npos) {
    const auto protocol = url.substr(0, protocol_end);
    result.m_is_https = (protocol == "https");
    const size_t host_start = protocol_end + 3;

    if (host_start >= url.length()) {
      throw std::runtime_error(std::format("Invalid URL: no host after protocol - {}", url));
    }

    const auto port_start = url.find(':', host_start);
    const auto path_start = url.find('/', host_start);

    if (port_start != std::string_view::npos &&
        (path_start == std::string_view::npos || port_start < path_start)) {
      result.m_host = std::string(url.substr(host_start, port_start - host_start));
      const auto port_str = std::string(
          url.substr(port_start + 1, path_start != std::string_view::npos
                                         ? path_start - port_start - 1
                                         : std::string_view::npos));
      try {
        result.m_port = std::stoi(port_str);
      } catch (...) {
        result.m_port = result.m_is_https ? 443 : 80;
      }
      result.m_path =
          (path_start != std::string_view::npos) ? std::string(url.substr(path_start)) : "/";
    } else {
      result.m_host = std::string(url.substr(host_start, path_start != std::string_view::npos
                                                 ? path_start - host_start
                                                 : std::string_view::npos));
      result.m_port = result.m_is_https ? 443 : 80;
      result.m_path =
          (path_start != std::string_view::npos) ? std::string(url.substr(path_start)) : "/";
    }
  }

  if (result.m_host.empty() || result.m_path.empty()) {
    throw std::runtime_error(std::format("Invalid URL: {}", url));
  }

  return result;
}

std::string format_http_error(httplib::Error error, int timeout_seconds,
                              const std::string &operation) {
  auto error_msg =
      std::format("HTTP {} failed: {}", operation, std::to_underlying(error));

  if (error == httplib::Error::Read || error == httplib::Error::Write) {
    error_msg += std::format(" (timeout after {} seconds)", timeout_seconds);
  } else if (error == httplib::Error::Connection) {
    error_msg += " (connection failed)";
  } else if (error == httplib::Error::BindIPAddress) {
    error_msg += " (failed to bind IP address)";
  }

  return error_msg;
}

void setup_ssl_client(httplib::SSLClient &client, int timeout_seconds,
                      bool enable_cert_verification,
                      bool enable_hostname_verification,
                      const std::string &ca_cert_path,
                      bool enable_connection_reuse) {
  client.set_connection_timeout(5, 0);
  client.set_read_timeout(timeout_seconds, 0);
  client.set_write_timeout(timeout_seconds, 0);
  client.enable_server_certificate_verification(enable_cert_verification);
  client.enable_server_hostname_verification(enable_hostname_verification);

  if (!ca_cert_path.empty()) {
    client.set_ca_cert_path(ca_cert_path);
  }

  if (enable_connection_reuse) {
    client.set_keep_alive(true);
    client.set_tcp_nodelay(true);
  }
}

void validate_trace_context(const TraceContext &ctx,
                            const std::string &context) {
  const std::pair<std::string_view, std::string_view> fields[] = {
      {"trace_id", ctx.m_trace_id},
      {"parent_id", ctx.m_parent_id},
      {"span_id", ctx.m_span_id},
      {"traceparent_header", ctx.m_traceparent_header},
  };
  for (const auto &[name, value] : fields) {
    if (value.empty()) {
      Logger::warn("{} - {}: empty", context, name);
    }
  }
}

std::string extract_client_ip(const httplib::Request &req) {
  // Prefer X-Real-IP: the trusted reverse proxy (nginx) overwrites it
  // unconditionally with the real peer address, so it cannot be spoofed by
  // the client. X-Forwarded-For, in contrast, accumulates client-supplied
  // values (nginx uses $proxy_add_x_forwarded_for).
  const auto xri_it = req.headers.find("x-real-ip");
  if (xri_it != req.headers.end() && !xri_it->second.empty()) {
    return xri_it->second;
  }

  const auto xff_it = req.headers.find("x-forwarded-for");
  if (xff_it != req.headers.end() && !xff_it->second.empty()) {
    const std::string &xff = xff_it->second;
    // Take the last address: the one appended by the trusted proxy closest to
    // the backend (leftmost entries may be client-supplied).
    const size_t comma_pos = xff.rfind(',');
    const auto client_ip =
        comma_pos == std::string::npos ? xff : xff.substr(comma_pos + 1);
    const size_t start = client_ip.find_first_not_of(" \t");
    const size_t end = client_ip.find_last_not_of(" \t");
    if (start != std::string::npos && end != std::string::npos) {
      return client_ip.substr(start, end - start + 1);
    }
  }

  const auto cf_it = req.headers.find("cf-connecting-ip");
  if (cf_it != req.headers.end() && !cf_it->second.empty()) {
    return cf_it->second;
  }

  return req.remote_addr;
}

std::string extract_query_string(const httplib::Request &req) {
  const std::string &target = req.target;
  const size_t q = target.find('?');
  if (q == std::string::npos) {
    return {};
  }
  return target.substr(q + 1);
}

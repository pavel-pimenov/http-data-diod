#ifndef ERROR_CATEGORIZER_HPP
#define ERROR_CATEGORIZER_HPP

#include <array>
#include <cctype>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Pure, dependency-light keyword-based error categorizers. Extracted from
// error_types.hpp / common_utils.cpp so the categorization logic can be unit
// tested without pulling in prometheus/httplib/JaegerLogger. The enums live in
// error_types.hpp; the categorizers here only map a message/status to a value.
//
// Rule semantics: the first rule whose keyword occurs in the lowercased message
// wins (mirrors the historic if/else-if chains).

namespace error_categorizer {

enum class HttpErrorType : std::uint8_t {
  CONNECTION_ERROR,
  SSL_ERROR,
  TIMEOUT_ERROR,
  STATUS_CODE_ERROR,
  PROTOCOL_ERROR,
  RATE_LIMIT_ERROR,
  SERVER_ERROR,
  CLIENT_ERROR,
  OTHER_ERROR
};

enum class L2ErrorType : std::uint8_t {
  CONNECTION_ERROR,
  TIMEOUT_ERROR,
  RESPONSE_ERROR,
  RETRY_EXHAUSTED,
  OTHER_ERROR
};

enum class ProcessingErrorType : std::uint8_t {
  JSON_PARSE_ERROR,
  VALIDATION_ERROR,
  DECOMPRESSION_ERROR,
  ENCODING_ERROR,
  TIMEOUT_ERROR,
  RESOURCE_EXHAUSTED,
  OTHER_ERROR
};

inline std::string to_lower(std::string_view s) {
  std::string lower;
  lower.reserve(s.size());
  for (const unsigned char c : s) {
    lower.push_back(static_cast<char>(std::tolower(c)));
  }
  return lower;
}

// Maps an enum value to its string name via an array indexed by the underlying
// value; unknown values fall back to "UNKNOWN".
template <typename Enum, size_t N>
inline std::string_view enum_to_string(Enum value,
                                       const std::array<const char *, N> &names) {
  const auto idx = static_cast<size_t>(value);
  if (idx < N && names[idx] != nullptr) {
    return names[idx];
  }
  return "UNKNOWN";
}

template <typename Enum> struct ErrorCategoryRule {
  Enum m_type;
  std::span<const std::string_view> m_keywords;
};

template <typename Enum, size_t N>
inline Enum categorize_by_keywords(
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

// Keyword tables for the categorizers. First matching rule wins.
inline constexpr std::array<std::string_view, 3> g_http_connection_keywords = {
    "connection", "could not resolve", "dns"};
inline constexpr std::array<std::string_view, 4> g_http_ssl_keywords = {
    "ssl", "tls", "certificate", "handshake"};
inline constexpr std::array<std::string_view, 2> g_http_timeout_keywords = {
    "timeout", "timed out"};
inline constexpr std::array<std::string_view, 2> g_http_protocol_keywords = {
    "protocol", "invalid"};
inline constexpr std::array<std::string_view, 2> g_l2_connection_keywords = {
    "connection", "could not resolve"};
inline constexpr std::array<std::string_view, 2> g_l2_timeout_keywords = {
    "timeout", "timed out"};
inline constexpr std::array<std::string_view, 2> g_l2_retry_keywords = {
    "retry", "attempts"};
inline constexpr std::array<std::string_view, 3> g_l2_response_keywords = {
    "json", "parse", "invalid"};
inline constexpr std::array<std::string_view, 2> g_processing_json_keywords = {
    "json", "parse"};
inline constexpr std::array<std::string_view, 3> g_processing_validation_keywords =
    {"valid", "required", "missing"};
inline constexpr std::array<std::string_view, 3>
    g_processing_decompression_keywords = {"gzip", "decompress", "inflate"};
inline constexpr std::array<std::string_view, 3> g_processing_encoding_keywords =
    {"base64", "encode", "decode"};
inline constexpr std::array<std::string_view, 1> g_processing_timeout_keywords = {
    "timeout"};
inline constexpr std::array<std::string_view, 2> g_processing_resource_keywords = {
    "exhausted", "no available"};

inline constexpr std::array g_http_keyword_rules = {
    ErrorCategoryRule<HttpErrorType>{HttpErrorType::CONNECTION_ERROR,
                                     g_http_connection_keywords},
    ErrorCategoryRule<HttpErrorType>{HttpErrorType::SSL_ERROR,
                                     g_http_ssl_keywords},
    ErrorCategoryRule<HttpErrorType>{HttpErrorType::TIMEOUT_ERROR,
                                     g_http_timeout_keywords},
    ErrorCategoryRule<HttpErrorType>{HttpErrorType::PROTOCOL_ERROR,
                                     g_http_protocol_keywords}};
inline constexpr std::array g_l2_keyword_rules = {
    ErrorCategoryRule<L2ErrorType>{L2ErrorType::CONNECTION_ERROR,
                                   g_l2_connection_keywords},
    ErrorCategoryRule<L2ErrorType>{L2ErrorType::TIMEOUT_ERROR,
                                   g_l2_timeout_keywords},
    ErrorCategoryRule<L2ErrorType>{L2ErrorType::RETRY_EXHAUSTED,
                                   g_l2_retry_keywords},
    ErrorCategoryRule<L2ErrorType>{L2ErrorType::RESPONSE_ERROR,
                                   g_l2_response_keywords}};
inline constexpr std::array g_processing_keyword_rules = {
    ErrorCategoryRule<ProcessingErrorType>{
        ProcessingErrorType::JSON_PARSE_ERROR, g_processing_json_keywords},
    ErrorCategoryRule<ProcessingErrorType>{
        ProcessingErrorType::VALIDATION_ERROR, g_processing_validation_keywords},
    ErrorCategoryRule<ProcessingErrorType>{
        ProcessingErrorType::DECOMPRESSION_ERROR,
        g_processing_decompression_keywords},
    ErrorCategoryRule<ProcessingErrorType>{ProcessingErrorType::ENCODING_ERROR,
                                           g_processing_encoding_keywords},
    ErrorCategoryRule<ProcessingErrorType>{ProcessingErrorType::TIMEOUT_ERROR,
                                           g_processing_timeout_keywords},
    ErrorCategoryRule<ProcessingErrorType>{
        ProcessingErrorType::RESOURCE_EXHAUSTED, g_processing_resource_keywords}};

inline constexpr std::array<const char *, 9> g_http_error_names = {
    "CONNECTION_ERROR",  "SSL_ERROR",      "TIMEOUT_ERROR",
    "STATUS_CODE_ERROR", "PROTOCOL_ERROR", "RATE_LIMIT_ERROR",
    "SERVER_ERROR",      "CLIENT_ERROR",   "OTHER_ERROR"};
inline constexpr std::array<const char *, 5> g_l2_error_names = {
    "CONNECTION_ERROR", "TIMEOUT_ERROR", "RESPONSE_ERROR", "RETRY_EXHAUSTED",
    "OTHER_ERROR"};
inline constexpr std::array<const char *, 7> g_processing_error_names = {
    "JSON_PARSE_ERROR", "VALIDATION_ERROR", "DECOMPRESSION_ERROR",
    "ENCODING_ERROR",   "TIMEOUT_ERROR",    "RESOURCE_EXHAUSTED",
    "OTHER_ERROR"};

inline HttpErrorType categorize_http_error(const std::string &error_msg,
                                           int status_code = 0) {
  if (status_code >= 500) {
    return HttpErrorType::SERVER_ERROR;
  }
  if (status_code == 429) {
    return HttpErrorType::RATE_LIMIT_ERROR;
  }
  if (status_code >= 400) {
    return HttpErrorType::CLIENT_ERROR;
  }
  return categorize_by_keywords(to_lower(error_msg), g_http_keyword_rules);
}

inline L2ErrorType categorize_l2_error(const std::string &error_msg) {
  return categorize_by_keywords(to_lower(error_msg), g_l2_keyword_rules);
}

inline ProcessingErrorType categorize_processing_error(
    const std::string &error_msg) {
  return categorize_by_keywords(to_lower(error_msg),
                                g_processing_keyword_rules);
}

inline std::string http_error_type_to_string(HttpErrorType type) {
  return std::string(enum_to_string(type, g_http_error_names));
}

inline std::string l2_error_type_to_string(L2ErrorType type) {
  return std::string(enum_to_string(type, g_l2_error_names));
}

inline std::string processing_error_type_to_string(ProcessingErrorType type) {
  return std::string(enum_to_string(type, g_processing_error_names));
}

} // namespace error_categorizer

#endif // ERROR_CATEGORIZER_HPP

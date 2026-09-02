#ifndef HEADER_UTILS_HPP
#define HEADER_UTILS_HPP

#include "httplib/httplib.h"
#include "logger.hpp"
#include <algorithm>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#if __has_include(<flat_set>)
#include <flat_set>
#endif

namespace header_utils {
#if __has_include(<flat_set>) && defined(__cpp_lib_flat_set)
using HeaderSet = std::flat_set<std::string>;
#else
using HeaderSet = std::set<std::string>;
#endif
// Inline namespace-scope singletons: guaranteed to be a single object across
// all translation units (C++17 inline variables), unlike function-local statics
// inside implicitly-inline member functions.
// flat_set for 4-10 elements: sorted vector, 0 allocations for find, cache-friendly
inline const HeaderSet g_default_skip_headers = {
    "host", "content-length", "connection", "transfer-encoding"};
inline const HeaderSet g_response_skip_headers = {
    "content-length", "transfer-encoding", "content-encoding"};
inline const HeaderSet g_sensitive_headers = {
    "authorization", "proxy-authorization",
    "cookie",        "set-cookie",
    "x-api-key",     "api-key",
    "x-apikey",      "apikey",
    "x-auth-token",  "x-access-token",
    "x-token",       "x-csrf-token",
    "x-xsrf-token",  "x-secret",
    "x-ws-secret",   "x-session-id",
    "session-id",    "jsessionid",
    "phpsessid",     "aspsessionid",
    "x-password",    "password",
    "passwd",        "x-credentials",
    "credentials",   "x-tenant-token",
    "authentication"};
// Substrings that mark a header name as sensitive even if it is not in the
// exact list above (e.g. x-amz-security-token, x-datadog-api-key).
inline const std::vector<std::string> g_sensitive_header_fragments = {
    "auth",     "token",  "secret", "key",        "cookie", "session",
    "password", "passwd", "pwd",    "credential", "csrf",   "xsrf"};
} // namespace header_utils

class HeaderUtils {
public:
  static const header_utils::HeaderSet &get_default_skip_headers() {
    return header_utils::g_default_skip_headers;
  }

  static const header_utils::HeaderSet &get_response_skip_headers() {
    return header_utils::g_response_skip_headers;
  }

  // Header names whose values must never appear in logs (credentials, tokens,
  // cookies). Values are still forwarded, only log lines are redacted.
  static const header_utils::HeaderSet &get_sensitive_headers() {
    return header_utils::g_sensitive_headers;
  }

  static const std::vector<std::string> &get_sensitive_header_fragments() {
    return header_utils::g_sensitive_header_fragments;
  }

  static bool is_sensitive_header(std::string_view header_name) {
    const auto lower = to_lower(header_name);
    if (get_sensitive_headers().contains(lower)) {
      return true;
    }
    return std::ranges::any_of(get_sensitive_header_fragments(),
                               [&](const auto &fragment) {
                                 return lower.contains(fragment);
                               });
  }

  // True for content types carrying opaque binary payloads (images, audio,
  // video, octet-stream). Shared by the worker (which base64-encodes such
  // responses) and the response path that decodes them.
  static bool is_binary_content_type(std::string_view content_type) {
    return content_type.contains("image/") ||
           content_type.contains("application/octet-stream") ||
           content_type.contains("audio/") ||
           content_type.contains("video/");
  }

  // Replace the value of sensitive headers in log lines with "***"
  static std::string redact_header_value(std::string_view header_name,
                                         std::string_view value) {
    return is_sensitive_header(header_name) ? "***"
                                            : std::string(value);
  }

  // Shared core of the filter_headers family: visits every (name, value) pair
  // produced by the source callback, logs skipped/forwarded decisions and
  // hands the non-skipped pairs to the emit callback. skip_headers is matched
  // case-insensitively.
  template <typename VisitFn, typename EmitFn>
  static void filter_headers_impl(const header_utils::HeaderSet &skip_headers,
                                  const std::string &log_context,
                                  VisitFn &&visit, EmitFn &&emit) {
    visit([&](const std::string &name, const std::string &value) {
      const auto lower_key = to_lower(name);
      if (skip_headers.contains(lower_key)) {
        Logger::debug("{} - Skipping header: {}", log_context, name);
      } else {
        emit(name, value);
        Logger::debug("{} - Forwarding header: {}: {}", log_context, name,
                      redact_header_value(name, value));
      }
    });
  }

public:
  template <typename SourceHeaders, typename DestHeaders>
  static void filter_headers(
      const SourceHeaders &source_headers, DestHeaders &dest_headers,
      const header_utils::HeaderSet &skip_headers = get_default_skip_headers(),
      const std::string &log_context = "HeaderFilter") {
    filter_headers_impl(
        skip_headers, log_context,
        [&source_headers](auto &&emit) {
          for (const auto &header : source_headers) {
            emit(header.first, header.second);
          }
        },
        [&dest_headers](const std::string &name, const std::string &value) {
          dest_headers.emplace(name, value);
        });
  }

  template <typename DestHeaders>
  static void filter_headers_from_json(
      const nlohmann::json &headers_json, DestHeaders &dest_headers,
      const header_utils::HeaderSet &skip_headers = get_default_skip_headers(),
      const std::string &log_context = "HeaderFilter") {
    if (!headers_json.is_object()) {
      return;
    }

    filter_headers_impl(
        skip_headers, log_context,
        [&headers_json](auto &&emit) {
          for (const auto &header : headers_json.items()) {
            emit(header.key(), header.value().get<std::string>());
          }
        },
        [&dest_headers](const std::string &name, const std::string &value) {
          dest_headers.emplace(name, value);
        });
  }

  static void filter_headers_to_json(
      const httplib::Headers &source_headers, nlohmann::json &dest_json,
      const header_utils::HeaderSet &skip_headers = get_default_skip_headers(),
      const std::string &log_context = "HeaderFilter") {
    dest_json = nlohmann::json::object();

    filter_headers_impl(
        skip_headers, log_context,
        [&source_headers](auto &&emit) {
          for (const auto &header : source_headers) {
            emit(header.first, header.second);
          }
        },
        [&dest_json](const std::string &name, const std::string &value) {
          dest_json[name] = value;
        });
  }

  static nlohmann::json
  headers_to_json(const httplib::Headers &source_headers) {
    nlohmann::json dest_json = nlohmann::json::object();
    for (const auto &header : source_headers) {
      dest_json[header.first] = header.second;
    }
    return dest_json;
  }

  static bool should_skip_header(
      std::string_view header_name,
      const header_utils::HeaderSet &skip_headers = get_default_skip_headers()) {
    return skip_headers.contains(to_lower(header_name));
  }

  static std::string to_lower(std::string_view header_name) {
    std::string lower;
    lower.reserve(header_name.size());
    for (const unsigned char c : header_name) {
      lower.push_back(static_cast<char>(std::tolower(c)));
    }
    return lower;
  }
};

#endif // HEADER_UTILS_HPP

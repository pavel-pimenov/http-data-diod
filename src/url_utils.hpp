#ifndef URL_UTILS_HPP
#define URL_UTILS_HPP

#include "httplib/httplib.h"
#include "logger.hpp"
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

struct ParsedUrl {
  std::string m_host;
  std::string m_path;
  int m_port = 0;
  bool m_is_https = false;
};

// Health-endpoint path constants, shared by the proxy and server handlers.
inline constexpr const char *kHealthLivePath = "/health/live";
inline constexpr const char *kHealthPath = "/health";
inline constexpr const char *kHealthReadyPath = "/health/ready";

// Header-only URL parser (like normalize_path / extract_client_ip above) so
// the pure routing helpers — including the SSRF-policy checks in l2_routing
// — stay unit-testable without linking common_utils.cpp.
[[nodiscard]] inline ParsedUrl parse_url(std::string_view url) {
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
        // Fallback: non-numeric port in URL → use default for scheme
        Logger::debug("URL port parse failed, using default for scheme");
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

// Prefer X-Real-IP: the trusted reverse proxy (nginx) overwrites it
// unconditionally with the real peer address, so it cannot be spoofed by
// the client. X-Forwarded-For, in contrast, accumulates client-supplied
// values (nginx uses $proxy_add_x_forwarded_for).
[[nodiscard]] inline std::string extract_client_ip(const httplib::Request &req) {
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

[[nodiscard]] inline std::string extract_query_string(const httplib::Request &req) {
  const std::string &target = req.target;
  const size_t q = target.find('?');
  if (q == std::string::npos) {
    return {};
  }
  return target.substr(q + 1);
}

inline std::string extract_proxy_ip(const httplib::Request &req) {
  return req.local_addr;
}

[[nodiscard]] inline std::string normalize_path(std::string_view path) {
  if (path.empty()) {
    return "/";
  }
  return (path[0] == '/') ? std::string(path) : std::format("/{}", path);
}

#endif // URL_UTILS_HPP

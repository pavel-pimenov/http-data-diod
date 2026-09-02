#ifndef URL_UTILS_HPP
#define URL_UTILS_HPP

#include "httplib/httplib.h"
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

[[nodiscard]] ParsedUrl parse_url(std::string_view url);

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

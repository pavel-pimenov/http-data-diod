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
[[nodiscard]] std::string extract_client_ip(const httplib::Request &req);
[[nodiscard]] std::string extract_query_string(const httplib::Request &req);

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

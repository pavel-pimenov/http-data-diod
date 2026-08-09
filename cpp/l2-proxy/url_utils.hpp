#ifndef URL_UTILS_HPP
#define URL_UTILS_HPP

#include "httplib/httplib.h"
#include <string>

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

ParsedUrl parse_url(const std::string &url);
std::string extract_client_ip(const httplib::Request &req);
std::string extract_query_string(const httplib::Request &req);

inline std::string extract_proxy_ip(const httplib::Request &req) {
  return req.local_addr;
}

inline std::string normalize_path(const std::string &path) {
  if (path.empty()) {
    return "/";
  }
  return (path[0] == '/') ? path : "/" + path;
}

#endif // URL_UTILS_HPP

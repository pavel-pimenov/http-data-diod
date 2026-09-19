#ifndef L2_ROUTING_HPP
#define L2_ROUTING_HPP

#include "url_utils.hpp"
#include <exception>
#include <string>
#include <string_view>
#include <vector>

// Pure helpers of the L2 worker's outbound routing: allowed-server lookup and
// URL construction. Header-only and free of AppContext/NATS/HttpClient so the
// proxy-core unit tests can cover the SSRF-policy contract (path allow/deny,
// segment boundary, dot-segment canonicalization, default-port omission)
// without booting the worker.

namespace l2_routing {

// RFC 3986 remove_dot_segments (URI path form, no scheme/authority): drops
// empty (collapsed '//'), "." and ".." segments, guarantees a leading '/'.
// Canonicalizing both the incoming request path and the configured base before
// the prefix comparison closes the "/api/../admin" alias and makes a base with
// a trailing slash ("/api/") match the same family as "/api".
[[nodiscard]] inline std::string canonicalize_path(std::string_view path) {
  if (path.empty()) {
    return "/";
  }
  std::string in(path);
  if (in.front() != '/') {
    in.insert(in.begin(), '/');
  }

  std::vector<std::string> segments;
  const std::string_view view(in);
  size_t cursor = 1; // skip the leading '/'
  while (cursor < view.size()) {
    const size_t slash = view.find('/', cursor);
    const size_t end = slash == std::string_view::npos ? view.size() : slash;
    const std::string_view segment = view.substr(cursor, end - cursor);
    if (segment.empty() || segment == ".") {
      // drop empty segments ('//') and '.' segments
    } else if (segment == "..") {
      if (!segments.empty()) {
        segments.pop_back();
      }
    } else {
      segments.emplace_back(segment);
    }
    cursor = end + 1;
  }

  std::string out;
  out.reserve(in.size());
  for (const std::string &segment : segments) {
    out += '/';
    out += segment;
  }
  if (out.empty()) {
    out = "/";
  }
  return out;
}

// scheme://host[:port] omitting the default http/https port, "" on a malformed
// URL. Used as the base of the outgoing L2 request URL.
[[nodiscard]] inline std::string extract_scheme_host_port(
    const std::string &url) {
  try {
    const ParsedUrl parsed = parse_url(url);
    std::string result = parsed.m_is_https ? "https://" : "http://";
    result += parsed.m_host;
    const int default_port = parsed.m_is_https ? 443 : 80;
    if (parsed.m_port != default_port) {
      result += ":" + std::to_string(parsed.m_port);
    }
    return result;
  } catch (const std::exception &e) {
    Logger::debug("Canonicalize failed for url '{}': {}", url, e.what());
    return "";
  }
}

// True when the canonicalized path maps to a configured L2 server; sets
// selected_url. "/", "/metrics" and "/favicon.ico" always map to the first
// server; otherwise the path must live under a configured base path (with a
// segment boundary, so "/api" accepts "/api/v1" but not "/apiv2").
[[nodiscard]] inline bool find_allowed_l2_server(
    const std::vector<std::string> &l2_server_urls, const std::string &path,
    std::string &selected_url) {
  if (l2_server_urls.empty()) {
    return false;
  }

  const std::string normalized_path = canonicalize_path(path);
  // Allow common paths that are safe to proxy on any configured backend
  if (normalized_path == "/metrics" || normalized_path == "/" ||
      normalized_path == "/favicon.ico") {
    selected_url = l2_server_urls[0];
    return true;
  }

  for (const auto &allowed_base : l2_server_urls) {
    try {
      const ParsedUrl parsed = parse_url(allowed_base);
      // Base canonicalized so a trailing slash does not break the boundary
      // match: "/api/" and "/api" accept the same family.
      const std::string base_path = canonicalize_path(parsed.m_path);
      // Base "/" matches any absolute path; otherwise require prefix match
      // with segment boundary ("/api" matches "/api/v1" but not "/apiv2")
      if (base_path == "/") {
        selected_url = allowed_base;
        return true;
      }
      if (normalized_path == base_path ||
          (normalized_path.rfind(base_path, 0) == 0 &&
           (normalized_path.size() == base_path.size() ||
            normalized_path[base_path.size()] == '/'))) {
        selected_url = allowed_base;
        return true;
      }
    } catch (const std::exception &e) {
      // Fallback: legacy prefix check on raw allowed_base string
      Logger::debug("Canonicalize failed for allowed_base '{}', legacy prefix "
                    "fallback: {}",
                    allowed_base, e.what());
      if (normalized_path.rfind(allowed_base, 0) == 0) {
        selected_url = allowed_base;
        return true;
      }
    }
  }

  return false;
}

// scheme://host[:port] of the selected server joined with the canonicalized
// path, without a double slash when the base ends with '/'. The path is
// canonicalized here (not only during the allow check) so the URL actually
// forwarded to the L2 server matches the policy decision.
[[nodiscard]] inline std::string build_l2_url(const std::string &selected_url,
                                              const std::string &path) {
  const std::string base_url = extract_scheme_host_port(selected_url);
  const std::string canonical_path = canonicalize_path(path);

  // canonicalize_path() guarantees a non-empty path starting with '/', so only
  // the base_url trailing slash decides whether a double slash would occur.
  std::string url;
  url.reserve(base_url.length() + canonical_path.length());
  if (!base_url.empty() && base_url.back() == '/') {
    url = base_url.substr(0, base_url.length() - 1) + canonical_path;
  } else {
    url = base_url + canonical_path;
  }
  return url;
}

} // namespace l2_routing

#endif // L2_ROUTING_HPP
#ifndef CRASH_DSN_HPP
#define CRASH_DSN_HPP

#include "constants.hpp"
#include "port_utils.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Sentry DSN parsing (scheme://key@host[:port]/project) kept free of the crash
// handler so it can be unit-tested without installing signal handlers. A
// malformed DSN must never throw out of CrashHandler::install(): a bad port
// falls back to the scheme default, a DSN without scheme/key yields an empty
// host, which the caller treats as "Sentry disabled".
namespace crash_dsn {

struct Parsed {
  std::string m_dsn_raw;
  std::string m_host;
  std::uint16_t m_port = 0;
  std::string m_key;
  std::string m_project;
};

inline Parsed parse_sentry_dsn(std::string_view dsn) {
  Parsed parsed;
  parsed.m_dsn_raw = std::string(dsn);
  const auto scheme_end = dsn.find("://");
  if (scheme_end == std::string_view::npos) {
    return parsed;
  }
  const std::size_t start =
      scheme_end + url_constants::kSchemeSeparatorLength;
  const auto at_pos = dsn.find('@', start);
  if (at_pos == std::string_view::npos) {
    return parsed;
  }
  const auto default_port = static_cast<std::uint16_t>(
      dsn.substr(0, scheme_end) == "https" ? net_limits::kHttpsPort
                                           : net_limits::kHttpPort);
  parsed.m_key = std::string(dsn.substr(start, at_pos - start));
  const std::size_t host_start = at_pos + 1;
  const auto slash_pos = dsn.find('/', host_start);
  const std::string_view host_port =
      slash_pos == std::string_view::npos
          ? dsn.substr(host_start)
          : dsn.substr(host_start, slash_pos - host_start);
  const auto colon = host_port.find(':');
  if (colon != std::string_view::npos) {
    parsed.m_host = std::string(host_port.substr(0, colon));
    const auto port = port_utils::parse_tcp_port(host_port.substr(colon + 1));
    parsed.m_port = port ? static_cast<std::uint16_t>(*port) : default_port;
  } else {
    parsed.m_host = std::string(host_port);
    parsed.m_port = default_port;
  }
  if (slash_pos != std::string_view::npos) {
    parsed.m_project = std::string(dsn.substr(slash_pos + 1));
  }
  return parsed;
}

} // namespace crash_dsn

#endif // CRASH_DSN_HPP

#ifndef PORT_UTILS_HPP
#define PORT_UTILS_HPP

#include "constants.hpp"
#include <charconv>
#include <optional>
#include <string_view>

// Strict decimal TCP-port parsing shared by the URL parser and the Sentry DSN
// parser. std::stoi accepted trailing garbage ("8080abc"), returned negative
// values and overflowed int → uint16_t silently; here anything that is not a
// plain decimal number in [kMinTcpPort, kMaxTcpPort] is rejected so the caller
// can fall back to the scheme default explicitly.
namespace port_utils {

inline std::optional<int> parse_tcp_port(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  int value = 0;
  const char *first = text.data();
  const char *last = first + text.size();
  const auto result = std::from_chars(first, last, value);
  if (result.ec != std::errc() || result.ptr != last) {
    return std::nullopt;
  }
  if (value < net_limits::kMinTcpPort || value > net_limits::kMaxTcpPort) {
    return std::nullopt;
  }
  return value;
}

} // namespace port_utils

#endif // PORT_UTILS_HPP

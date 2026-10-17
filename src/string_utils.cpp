#include "string_utils.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

[[nodiscard]] std::string to_lower(std::string_view s) {
  std::string lower;
  lower.reserve(s.size());
  for (const unsigned char c : s) {
    lower.push_back(static_cast<char>(std::tolower(c)));
  }
  return lower;
} // LCOV_EXCL_LINE

[[nodiscard]] std::string trim_trailing(std::string_view s,
                                        std::string_view chars) {
  std::size_t end = s.size();
  while (end > 0 && chars.find(s[end - 1]) != std::string_view::npos) {
    --end;
  }
  return std::string(s.substr(0, end));
} // LCOV_EXCL_LINE
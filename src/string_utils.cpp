#include "string_utils.hpp"

#include <cctype>

[[nodiscard]] std::string to_lower(std::string_view s) {
  std::string lower;
  lower.reserve(s.size());
  for (const unsigned char c : s) {
    lower.push_back(static_cast<char>(std::tolower(c)));
  }
  return lower;
} // LCOV_EXCL_LINE
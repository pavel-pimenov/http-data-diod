#ifndef STRING_UTILS_HPP
#define STRING_UTILS_HPP

#include <cctype>
#include <string>
#include <string_view>

// Shared ASCII lowercasing. Centralises the identical byte-wise tolower loop
// that was duplicated in error_categorizer.hpp, header_utils.hpp, config.cpp
// and db_query_utils.hpp.
[[nodiscard]] inline std::string to_lower(std::string_view s) {
  std::string lower;
  lower.reserve(s.size());
  for (const unsigned char c : s) {
    lower.push_back(static_cast<char>(std::tolower(c)));
  }
  return lower;
}

#endif // STRING_UTILS_HPP

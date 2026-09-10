#ifndef STRING_UTILS_HPP
#define STRING_UTILS_HPP

#include <string>
#include <string_view>

// Shared ASCII lowercasing. Centralises the identical byte-wise tolower loop
// that was duplicated in error_categorizer.hpp, header_utils.hpp, config.cpp
// and db_query_utils.hpp. Implementation lives in string_utils.cpp (keeps the
// closing-brace gcov artifact of the fully-inlined inline version from
// reporting the header line as uncovered).
[[nodiscard]] std::string to_lower(std::string_view s);

#endif // STRING_UTILS_HPP
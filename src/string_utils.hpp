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

// Trims trailing characters (CR/LF by default) from `s`. Centralises the
// identical byte loop that was duplicated in crash_utils::trim_line (addr2line
// output cleanup) and the Postgres executor's trim_copy (libpq error text,
// which also strips a trailing space).
[[nodiscard]] std::string trim_trailing(std::string_view s,
                                        std::string_view chars = "\r\n");

#endif // STRING_UTILS_HPP
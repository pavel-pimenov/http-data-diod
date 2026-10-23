#ifndef CRASH_DUMP_COUNTER_HPP
#define CRASH_DUMP_COUNTER_HPP

#include "logger.hpp"

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

// Counts the crash dumps the handler (crash_handler.hpp) left in the dump
// directory. The name format is shared: crash_<timestamp>_<SIGNAL>.txt.
namespace crash_dump_counter {

inline constexpr std::string_view g_file_prefix = "crash_";
inline constexpr std::string_view g_file_suffix = ".txt";

// The timestamp starts right after the prefix with a digit. The test marker
// (crash_test_marker.txt) also carries the prefix, so it is excluded by that
// rule rather than counted as a dump.
inline bool is_dump_file_name(const std::string &name) {
  if (name.size() <= g_file_prefix.size() + g_file_suffix.size() + 1 ||
      !name.starts_with(g_file_prefix) || !name.ends_with(g_file_suffix)) {
    return false;
  }
  return std::isdigit(static_cast<unsigned char>(name[g_file_prefix.size()])) !=
         0;
}

// A missing or unreadable directory is not an error: it means no dumps have
// been written yet (the crash handler creates the directory on install).
inline std::size_t count_files(const std::string &dir) {
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) {
    return 0;
  }
  const std::filesystem::directory_iterator end;
  std::size_t count = 0;
  while (it != end) {
    if (is_dump_file_name(it->path().filename().string())) {
      ++count;
    }
    it.increment(ec);
    if (ec) {
      Logger::debug("Crash dump directory {} stopped listing: {}", dir,
                    ec.message());
      return count;
    }
  }
  return count;
}

} // namespace crash_dump_counter

#endif // CRASH_DUMP_COUNTER_HPP

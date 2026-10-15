#ifndef TEST_PG_DIAG_HPP
#define TEST_PG_DIAG_HPP

// Shared diagnostics for the two ephemeral-postgres test harnesses
// (test_db_live.cpp and the pgtest block of test_nats_live.cpp). Both used to
// fail silently when the environment could not bring a cluster up (host disk
// full, missing binaries, ...): the tests degraded to an opaque SKIP and the
// coverage gate failed without a hint why. Here the reason is always reported,
// and a cluster is never attempted when /tmp cannot hold it.

#include "logger.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <sys/statvfs.h>

namespace pg_test_diag {

inline constexpr uint64_t kMinFreeBytes = 256ULL * 1024ULL * 1024ULL;

[[nodiscard]] inline uint64_t free_bytes(const char *path) {
  struct statvfs vfs {};
  if (::statvfs(path, &vfs) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(vfs.f_bavail) *
         static_cast<uint64_t>(vfs.f_frsize);
}

// Reasons go to the logger and to stderr: spdlog's sinks write to a file too and
// can themselves fail when the disk is full - the very case that has to be
// explained instead of swallowed by a SKIP.
inline void fail(std::string_view reason) {
  Logger::error("pg live test: {}", reason);
  std::fprintf(stderr, "[pg live test] %.*s\n", static_cast<int>(reason.size()),
               reason.data());
  std::fflush(stderr);
}

[[nodiscard]] inline bool enough_free_space(const char *path = "/tmp") {
  const uint64_t free = free_bytes(path);
  if (free < kMinFreeBytes) {
    fail(std::format(
        "only {} MiB free on {} (need {} MiB) - cannot start a postgres "
        "cluster for the live DB tests",
        free / (1024ULL * 1024ULL), path, kMinFreeBytes / (1024ULL * 1024ULL)));
    return false;
  }
  return true;
}

// Tail of a server/initdb log, newlines flattened so the build log stays
// readable. This is what turns "the cluster did not come up" into the actual
// postgres error (e.g. "no space left on device").
[[nodiscard]] inline std::string log_tail(const std::string &path,
                                          size_t max_bytes = 600) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return "<no log>";
  }
  in.seekg(0, std::ios::end);
  const std::streamoff end = in.tellg();
  if (end <= 0) {
    return "<empty log>";
  }
  const auto size = static_cast<size_t>(end);
  const size_t take = std::min(max_bytes, size);
  in.seekg(static_cast<std::streamoff>(size - take));
  std::string out(take, '\0');
  in.read(out.data(), static_cast<std::streamsize>(take));
  out.resize(static_cast<size_t>(in.gcount()));
  std::ranges::replace(out, '\n', ' ');
  return out;
}

} // namespace pg_test_diag

#endif // TEST_PG_DIAG_HPP

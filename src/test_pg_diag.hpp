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
#include <fcntl.h>
#include <format>
#include <fstream>
#include <grp.h>
#include <ios>
#include <libpq-fe.h>
#include <pwd.h>
#include <string>
#include <string_view>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

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

// Unix user a forked postgres must run as: an existing non-root user (postgres,
// or the current user when the test already runs unprivileged). Root is refused
// by postgres, so the harness has to switch before exec.
struct ServerUser {
  bool m_ok = false;
  bool m_need_switch = false;
  uid_t m_uid = 0;
  gid_t m_gid = 0;
  std::string m_name;
};

[[nodiscard]] inline ServerUser resolve_server_user() {
  if (::geteuid() != 0) {
    return ServerUser{true, false, ::geteuid(), ::getegid(), ""};
  }
  const struct passwd *pw = ::getpwnam("postgres");
  if (pw == nullptr) {
    Logger::warn("pg live test: no 'postgres' user in this image, skipping");
    return ServerUser{};
  }
  return ServerUser{true, true, pw->pw_uid, pw->pw_gid,
                    std::string(pw->pw_name)};
}

[[nodiscard]] inline bool drop_privileges(const ServerUser &user) {
  if (!user.m_need_switch) {
    return true;
  }
  if (::setgid(user.m_gid) != 0) {
    return false;
  }
  if (::initgroups(user.m_name.c_str(), user.m_gid) != 0) {
    return false;
  }
  return ::setuid(user.m_uid) == 0;
}

// Point the forked server's stdout/stderr at the harness log, so a failed
// cluster start can be explained by log_tail() instead of vanishing.
[[nodiscard]] inline bool redirect_to(const char *path) {
  const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return false;
  }
  ::dup2(fd, 1);
  ::dup2(fd, 2);
  if (fd > 2) {
    ::close(fd);
  }
  return true;
}

// A bare TCP accept only proves the postmaster bound the port: right after the
// start it still answers SQL connections with "the database system is starting
// up". Probe with libpq so that "ready" really means an accepted SQL session.
[[nodiscard]] inline bool postgres_sql_probe(int port) {
  const std::string conninfo = std::format(
      "host=127.0.0.1 port={} dbname=postgres user=postgres connect_timeout=1",
      port);
  PGconn *conn = PQconnectdb(conninfo.c_str());
  if (conn == nullptr) {
    return false;
  }
  const bool ok = PQstatus(conn) == CONNECTION_OK;
  PQfinish(conn);
  return ok;
}

} // namespace pg_test_diag

#endif // TEST_PG_DIAG_HPP

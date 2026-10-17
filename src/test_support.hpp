#ifndef TEST_SUPPORT_HPP
#define TEST_SUPPORT_HPP

#include "app_context.hpp"
#include "httplib/httplib.h"
#include "request_handler.hpp"
#include "stats_logger.hpp"
#include "trace_logger.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <stdlib.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// Helpers shared by the unit-test translation units: scoped env-var overrides
// and the tracer stub. Test-only — the `test_` file-name prefix keeps the file
// out of the coverage gate — so the bodies live in one place instead of being
// copied into every test TU that needs them.

namespace test_support {

[[nodiscard]] inline std::optional<std::string> read_env(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
}

using EnvVars = std::map<std::string, std::optional<std::string>>;

// RAII override of several env vars: remembers the previous value (set or
// unset) and restores it on scope exit so one test cannot leak state into the
// next one. Holds the shared storage/restore logic; the subclasses differ only
// in how the overrides are supplied.
class EnvScope {
public:
  ~EnvScope() { restore(); }

  EnvScope(const EnvScope &) = delete;
  EnvScope &operator=(const EnvScope &) = delete;
  EnvScope(EnvScope &&) = delete;
  EnvScope &operator=(EnvScope &&) = delete;

protected:
  EnvScope() = default;

  void apply(const EnvVars &vars) {
    m_vars.reserve(vars.size());
    for (const auto &[name, value] : vars) {
      m_vars.emplace_back(name, read_env(name.c_str()));
      if (value.has_value()) {
        setenv(name.c_str(), value->c_str(), 1);
      } else {
        unsetenv(name.c_str());
      }
    }
  }

private:
  void restore() {
    for (const auto &[name, previous] : m_vars) {
      if (previous.has_value()) {
        setenv(name.c_str(), previous->c_str(), 1);
      } else {
        unsetenv(name.c_str());
      }
    }
  }

  std::vector<std::pair<std::string, std::optional<std::string>>> m_vars;
};

// Braced form for literal overrides: EnvGuard guard{{"MODE", "proxy"}}. A null
// value unsets the variable.
class EnvGuard : public EnvScope {
public:
  EnvGuard(std::initializer_list<std::pair<const char *, const char *>> vars) {
    EnvVars mapped;
    for (const auto &[name, value] : vars) {
      mapped[name] = value != nullptr ? std::optional<std::string>(value)
                                      : std::nullopt;
    }
    apply(mapped);
  }
};

// Map form for overrides built dynamically: EnvMapGuard guard(some_env()).
class EnvMapGuard : public EnvScope {
public:
  explicit EnvMapGuard(const EnvVars &vars) { apply(vars); }
};

// Batched Jaeger exporter pointed at a refused port: spans are buffered and the
// flush fails harmlessly, so the tracer-dependent branches become reachable
// without an observability backend.
inline void attach_tracer(AppContext &ctx) {
  auto &metrics = *ctx.m_tracing_metrics;
  ctx.m_tracer = std::make_unique<JaegerLogger>(
      "http://127.0.0.1:1/api/traces", metrics.m_spans_sent,
      metrics.m_spans_failed, metrics.m_queue_size, metrics.m_last_send_duration,
      metrics.m_send_latency, metrics.m_queue_time,
      /*batch_size=*/10000, /*flush_interval_ms=*/100000, /*sample_rate=*/1.0);
}

inline httplib::Request make_get(const std::string &path) {
  httplib::Request req;
  req.method = "GET";
  req.path = path;
  return req;
}

inline httplib::Request make_post(const std::string &path,
                                  const std::string &body) {
  httplib::Request req;
  req.method = "POST";
  req.path = path;
  req.body = body;
  return req;
}

// Reserve an ephemeral loopback TCP port: bind to port 0, read the port the
// kernel picked, release it. The live-server fixtures (nats-server, postgres)
// need a port that is free right now before they fork the real server onto it.
[[nodiscard]] inline int reserve_loopback_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
  }
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

// Poll `predicate` every 50 ms until it returns true or `timeout_ms` elapses;
// the predicate is checked once more after the deadline so a condition that
// became true just as the timeout expired still counts. Keeps the live-test
// waits bounded even when the server binary is missing or the broker hangs.
[[nodiscard]] inline bool wait_for_condition(
    const std::function<bool()> &predicate, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return predicate();
}

// The three objects every RequestHandler-driven test needs together: the
// shutdown flag, the stats logger that borrows it and the handler that borrows
// the logger. Declaration order is construction order and the handler is
// destroyed first, so no reference outlives its target. The handle_* forwarders
// let a test body keep calling them as if it held a RequestHandler directly.
class ProxyHandler {
public:
  explicit ProxyHandler(AppContext &ctx)
      : m_stats(ctx, m_shutdown), m_handler(ctx, m_stats) {}

  void handle_get(const httplib::Request &req, httplib::Response &res) {
    m_handler.handle_get(req, res);
  }

  void handle_post(const httplib::Request &req, httplib::Response &res) {
    m_handler.handle_post(req, res);
  }

private:
  std::atomic<bool> m_shutdown{false};
  StatsLogger m_stats;
  RequestHandler m_handler;
};

} // namespace test_support

#endif // TEST_SUPPORT_HPP

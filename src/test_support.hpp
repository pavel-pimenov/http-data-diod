#ifndef TEST_SUPPORT_HPP
#define TEST_SUPPORT_HPP

#include "app_context.hpp"
#include "httplib/httplib.h"
#include "trace_logger.hpp"

#include <cstdlib>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <stdlib.h>
#include <string>
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

} // namespace test_support

#endif // TEST_SUPPORT_HPP

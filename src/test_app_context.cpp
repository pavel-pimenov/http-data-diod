#include "app_context.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

class AppCtxEnvGuard {
public:
  AppCtxEnvGuard(const char *name, const char *value) : m_name(name) {
    if (value == nullptr) {
      unsetenv(m_name.c_str());
    } else {
      setenv(m_name.c_str(), value, 1);
    }
  }
  ~AppCtxEnvGuard() { unsetenv(m_name.c_str()); }

private:
  std::string m_name;
};

std::vector<std::string> family_names(
    const std::shared_ptr<prometheus::Registry> &registry) {
  std::vector<std::string> names;
  for (const auto &family : registry->Collect()) {
    names.push_back(family.name);
  }
  return names;
}

bool has_family(const std::shared_ptr<prometheus::Registry> &registry,
                const std::string &name) {
  const auto names = family_names(registry);
  return std::find(names.begin(), names.end(), name) != names.end();
}

const std::vector<std::string> &common_l2_families() {
  static const std::vector<std::string> g_k_families{
      "l2_tracing_spans_sent_total",
      "l2_tracing_spans_failed_total",
      "l2_tracing_queue_size",
      "l2_tracing_last_send_duration_seconds",
      "l2_tracing_send_latency_seconds",
      "l2_tracing_queue_time_seconds",
      "l2_worker_sentry_events_sent_total",
      "l2_worker_sentry_events_failed_total",
      "l2_worker_sentry_queue_size",
  };
  return g_k_families;
}

void require_common_registry_only_on_common(AppContext &ctx) {
  for (const auto &name : common_l2_families()) {
    REQUIRE(has_family(ctx.m_common_registry, name));
  }
  // The cross-cutting tracing/sentry metrics must NOT leak into the per-mode
  // registries (they are registered once on the shared common registry only).
  for (const auto &name : common_l2_families()) {
    REQUIRE_FALSE(has_family(ctx.m_proxy_registry, name));
    REQUIRE_FALSE(has_family(ctx.m_worker_registry, name));
    REQUIRE_FALSE(has_family(ctx.m_server_registry, name));
  }
}

} // namespace

TEST_CASE("AppContext: worker mode builds l2_common registry and mode stats",
          "[app_context]") {
  AppCtxEnvGuard mode("MODE", "worker");

  AppContext ctx;

  REQUIRE(ctx.m_proxy_registry != nullptr);
  REQUIRE(ctx.m_worker_registry != nullptr);
  REQUIRE(ctx.m_server_registry != nullptr);
  REQUIRE(ctx.m_common_registry != nullptr);
  // AppContext is fully constructible without the NATS client: proxy runtime
  // components are initialized later by init_proxy_components() in run_proxy.
  REQUIRE(ctx.m_nats_client == nullptr);

  REQUIRE(ctx.m_worker.m_metrics != nullptr);
  REQUIRE(ctx.m_proxy.m_metrics != nullptr);
  REQUIRE(ctx.m_server.m_metrics != nullptr);
  REQUIRE(ctx.m_tracing_metrics != nullptr);
  REQUIRE(ctx.m_sentry_metrics != nullptr);

  REQUIRE(ctx.m_common_stats_history != nullptr);
  require_common_registry_only_on_common(ctx);

  REQUIRE(ctx.m_config.m_mode == "worker");
}

TEST_CASE("AppContext: l2-server mode builds l2_common registry as well",
          "[app_context]") {
  AppCtxEnvGuard mode("MODE", "l2-server");

  AppContext ctx;

  REQUIRE(ctx.m_common_registry != nullptr);
  require_common_registry_only_on_common(ctx);
  REQUIRE(ctx.m_server.m_metrics != nullptr);
  REQUIRE(ctx.m_config.m_mode == "l2-server");
}
#include "app_context.hpp"
#include "test_support.hpp"
#include "prometheus/registry.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <stdlib.h>
#include <memory>
#include <set>
#include <map>
#include <string>
#include <vector>

namespace {

using test_support::EnvGuard;

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
  return std::ranges::find(names, name) != names.end();
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
      "l2_common_build_info",
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
  EnvGuard const mode{{"MODE", "worker"}};

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

  REQUIRE(ctx.m_config.m_app.m_mode == "worker");
}

TEST_CASE("AppContext: l2-server mode builds l2_common registry as well",
          "[app_context]") {
  EnvGuard const mode{{"MODE", "l2-server"}};

  AppContext ctx;

  REQUIRE(ctx.m_common_registry != nullptr);
  require_common_registry_only_on_common(ctx);
  REQUIRE(ctx.m_server.m_metrics != nullptr);
  REQUIRE(ctx.m_config.m_app.m_mode == "l2-server");
}

TEST_CASE("AppContext: worker pre-creates the L2 call error type series",
          "[app_context]") {
  EnvGuard const mode{{"MODE", "worker"}};

  AppContext const ctx;

  REQUIRE(has_family(ctx.m_worker_registry, "l2_worker_l2_call_errors_total"));
  std::vector<std::string> types;
  for (const auto &family : ctx.m_worker_registry->Collect()) {
    if (family.name != "l2_worker_l2_call_errors_total") {
      continue;
    }
    for (const auto &metric : family.metric) {
      for (const auto &label : metric.label) {
        if (label.name == "type") {
          types.push_back(label.value);
        }
      }
    }
  }
  // prometheus-cpp exports a labeled family only after the first Add(), and a
  // healthy stack never fails an L2 call, so all enum values must exist as 0.
  std::ranges::sort(types);
  REQUIRE(types == std::vector<std::string>{"CONNECTION_ERROR", "OTHER_ERROR",
                                           "RESPONSE_ERROR", "RETRY_EXHAUSTED",
                                           "TIMEOUT_ERROR"});
}

TEST_CASE("AppContext: pre-creates the tracing span drop reason series",
          "[app_context]") {
  EnvGuard const mode{{"MODE", "worker"}};

  AppContext const ctx;

  REQUIRE(
      has_family(ctx.m_common_registry, "l2_tracing_spans_dropped_total"));
  std::vector<std::string> reasons;
  for (const auto &family : ctx.m_common_registry->Collect()) {
    if (family.name != "l2_tracing_spans_dropped_total") {
      continue;
    }
    for (const auto &metric : family.metric) {
      for (const auto &label : metric.label) {
        if (label.name == "reason") {
          reasons.push_back(label.value);
        }
      }
    }
  }
  // prometheus-cpp exports a labeled family only after the first Add(), and a
  // healthy stack never loses a span, so all reasons must exist as 0.
  std::ranges::sort(reasons);
  REQUIRE(reasons == std::vector<std::string>{"breaker_open", "export_error",
                                            "queue_full", "shutdown_flush"});
}

TEST_CASE("AppContext: proxy pre-creates the rejection reason series",
          "[app_context]") {
  EnvGuard const mode{{"MODE", "proxy"}};

  AppContext const ctx;

  REQUIRE(has_family(ctx.m_proxy_registry, "l2_proxy_rejections_total"));
  std::vector<std::string> reasons;
  for (const auto &family : ctx.m_proxy_registry->Collect()) {
    if (family.name != "l2_proxy_rejections_total") {
      continue;
    }
    for (const auto &metric : family.metric) {
      for (const auto &label : metric.label) {
        if (label.name == "reason") {
          reasons.push_back(label.value);
        }
      }
    }
  }
  // prometheus-cpp exports a labeled family only after the first Add(), and a
  // healthy proxy refuses nothing, so all reasons must exist as 0.
  std::ranges::sort(reasons);
  REQUIRE(reasons == std::vector<std::string>{"duplicate", "invalid_json",
                                            "rate_limit_global",
                                            "rate_limit_per_ip"});
}

TEST_CASE("AppContext: proxy pre-creates the duplicate eviction reason series",
          "[app_context]") {
  EnvGuard const mode{{"MODE", "proxy"}};

  AppContext const ctx;

  REQUIRE(has_family(ctx.m_proxy_registry,
                      "l2_proxy_duplicate_cache_evictions_total"));
  std::vector<std::string> reasons;
  for (const auto &family : ctx.m_proxy_registry->Collect()) {
    if (family.name != "l2_proxy_duplicate_cache_evictions_total") {
      continue;
    }
    for (const auto &metric : family.metric) {
      for (const auto &label : metric.label) {
        if (label.name == "reason") {
          reasons.push_back(label.value);
        }
      }
    }
  }
  // A healthy proxy never evicts anything, so all reasons must exist as 0.
  std::ranges::sort(reasons);
  REQUIRE(reasons == std::vector<std::string>{"body_cap", "body_ttl",
                                            "client_cap", "client_ttl"});
}

TEST_CASE("AppContext: worker pre-creates the validation field series",
          "[app_context]") {
  EnvGuard const mode{{"MODE", "worker"}};

  AppContext const ctx;

  REQUIRE(has_family(ctx.m_worker_registry,
                      "l2_worker_request_validation_errors_total"));
  std::vector<std::string> fields;
  for (const auto &family : ctx.m_worker_registry->Collect()) {
    if (family.name != "l2_worker_request_validation_errors_total") {
      continue;
    }
    for (const auto &metric : family.metric) {
      for (const auto &label : metric.label) {
        if (label.name == "field") {
          fields.push_back(label.value);
        }
      }
    }
  }
  // A healthy worker rejects nothing, so every field the validator can report
  // must exist as 0.
  std::ranges::sort(fields);
  REQUIRE(fields == std::vector<std::string>{"body", "method", "path",
                                             "request_id"});
}

TEST_CASE("AppContext: labelled families keep their label contract",
          "[app_context]") {
  // The dashboard legends (PromQL {{type}} / {{reason}} / {{outcome}} / ...) are
  // bound to these label names, and the metric structs are aggregate-initialised
  // positionally -- a mismatch between the declaration order and the
  // registration order swaps two families silently. This pins the label set of
  // every labelled family the dashboards rely on.
  const std::map<std::string, std::set<std::string>> expected{
      {"l2_proxy_client_request_errors_total", {"type"}},
      {"l2_proxy_rejections_total", {"reason"}},
      // The per-database families (l2_proxy_db_requests_total and
      // l2_worker_db_requests_total) are not listed here: their series appear
      // only after real DB traffic, so a fresh context does not export them.
      {"l2_worker_l2_call_errors_total", {"type"}},
      {"l2_worker_retries_total", {"outcome"}},
      {"l2_worker_db_query_errors_total", {"error_class"}},
      {"l2_tracing_spans_dropped_total", {"reason"}},
      {"l2_worker_sentry_events_dropped_total", {"reason"}},
      {"l2_proxy_not_ready_total", {"reason"}},
      {"l2_worker_not_ready_total", {"reason"}},
      {"l2_common_stats_history_samples_total", {"outcome"}},
      {"l2_proxy_duplicate_cache_evictions_total", {"reason"}},
      {"l2_worker_l2_server_call_duration_seconds", {"backend"}},
      {"l2_worker_request_validation_errors_total", {"field"}},
  };

  std::map<std::string, std::set<std::string>> actual;
  for (const char *mode : {"proxy", "worker"}) {
    EnvGuard const env{{"MODE", mode}};
    AppContext const ctx;

    for (const auto &registry :
         {ctx.m_proxy_registry, ctx.m_worker_registry, ctx.m_common_registry}) {
      for (const auto &family : registry->Collect()) {
        if (!expected.contains(family.name)) {
          continue;
        }
        std::set<std::string> labels;
        for (const auto &metric : family.metric) {
          for (const auto &label : metric.label) {
            labels.insert(label.name);
          }
        }
        auto [it, inserted] = actual.emplace(family.name, labels);
        if (!inserted) {
          // The same family must carry the same label set in every mode.
          REQUIRE(it->second == labels);
        }
      }
    }
  }

  // Every family of the contract must be present with exactly its labels.
  for (const auto &[name, labels] : expected) {
    REQUIRE(actual.contains(name));
    REQUIRE(actual.at(name) == labels);
  }
}

TEST_CASE("AppContext: the DB error family is labeled by error class",
          "[app_context]") {
  EnvGuard const mode{{"MODE", "worker"}};

  AppContext const ctx;

  // The metric structs are aggregate-initialised positionally, so a mismatch
  // between the declaration order and the registration order silently swaps
  // two families (and their label sets). This pins the label set of the error
  // family so such a swap cannot pass unnoticed.
  std::set<std::string> labels;
  for (const auto &family : ctx.m_worker_registry->Collect()) {
    if (family.name != "l2_worker_db_query_errors_total") {
      continue;
    }
    for (const auto &metric : family.metric) {
      for (const auto &label : metric.label) {
        labels.insert(label.name);
      }
    }
  }
  REQUIRE(labels == std::set<std::string>{"error_class"});
}

TEST_CASE("AppContext: build info carries the version and the mode",
          "[app_context]") {
  EnvGuard const mode{{"MODE", "worker"}};

  AppContext const ctx;

  bool found = false;
  std::map<std::string, std::string> labels;
  double value = 0.0;
  for (const auto &family : ctx.m_common_registry->Collect()) {
    if (family.name != "l2_common_build_info") {
      continue;
    }
    found = true;
    REQUIRE(family.metric.size() == 1);
    value = family.metric.front().gauge.value;
    for (const auto &label : family.metric.front().label) {
      labels[label.name] = label.value;
    }
  }
  REQUIRE(found);
  REQUIRE(value == 1.0);
  REQUIRE(labels.count("version") == 1);
  REQUIRE_FALSE(labels["version"].empty());
  REQUIRE(labels["mode"] == "worker");
}

TEST_CASE("AppContext: proxy pre-creates the client error type series",
          "[app_context]") {
  EnvGuard const mode{{"MODE", "proxy"}};

  AppContext const ctx;

  REQUIRE(
      has_family(ctx.m_proxy_registry, "l2_proxy_client_request_errors_total"));
  std::vector<std::string> types;
  for (const auto &family : ctx.m_proxy_registry->Collect()) {
    if (family.name != "l2_proxy_client_request_errors_total") {
      continue;
    }
    for (const auto &metric : family.metric) {
      for (const auto &label : metric.label) {
        if (label.name == "type") {
          types.push_back(label.value);
        }
      }
    }
  }
  // prometheus-cpp exports a labeled family only after the first Add(), and a
  // healthy proxy refuses nothing, so all enum values must exist as 0.
  std::ranges::sort(types);
  REQUIRE(types == std::vector<std::string>{
                      "CLIENT_ERROR",      "CONNECTION_ERROR",
                      "OTHER_ERROR",       "PROTOCOL_ERROR",
                      "RATE_LIMIT_ERROR",  "SERVER_ERROR",
                      "SSL_ERROR",         "STATUS_CODE_ERROR",
                      "TIMEOUT_ERROR"});
}
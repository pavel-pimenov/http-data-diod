#ifndef METRICS_MANAGER_HPP
#define METRICS_MANAGER_HPP

#include "time_utils.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string>
#include <utility>
#include <vector>

namespace histogram_buckets {
// Histogram bucket bounds grouped in one struct so the related latency/size
// tables stay together and are exposed through a single global instance.
struct Buckets {
  std::array<double, 11> m_latency_ms_to_5s;
  std::array<double, 12> m_latency_ms_to_10s;
  std::array<double, 11> m_latency_5ms_to_10s;
  std::array<double, 10> m_size_100b_to_5mb;
  // Sub-millisecond resolution for micro-latencies (task-queue wait, single
  // NATS round-trip): starts at 100us so "instant" and "queued" are separable.
  std::array<double, 13> m_latency_100us_to_1s;
};
inline constexpr Buckets g_buckets = {
    {{0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0}},
    {{0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0}},
    {{0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0}},
    {{100, 500, 1000, 5000, 10000, 50000, 100000, 500000, 1000000, 5000000}},
    {{0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1,
      0.25, 0.5, 1.0}}};
} // namespace histogram_buckets

// Histogram families need the bucket bounds on every Family::Add call (the
// family itself does not remember them). Returned by reference from one static
// so frequent observes do not rebuild the vector on every call.
inline const std::vector<double> &latency_buckets_ms_to_10s() {
  static const std::vector<double> g_latency_buckets = {
      histogram_buckets::g_buckets.m_latency_ms_to_10s.begin(),
      histogram_buckets::g_buckets.m_latency_ms_to_10s.end()};
  return g_latency_buckets;
}

namespace metrics_detail {
// Maps a metric type to its prometheus-cpp builder, so the create_* wrappers
// share one implementation instead of repeating the builder chain.
template <typename Metric> struct Builder;
template <> struct Builder<prometheus::Counter> {
  [[nodiscard]] static auto of() { return prometheus::BuildCounter(); }
};
template <> struct Builder<prometheus::Gauge> {
  [[nodiscard]] static auto of() { return prometheus::BuildGauge(); }
};
template <> struct Builder<prometheus::Histogram> {
  [[nodiscard]] static auto of() { return prometheus::BuildHistogram(); }
};
} // namespace metrics_detail

class MetricsManager {
public:
  static prometheus::Counter &
  create_counter(const std::shared_ptr<prometheus::Registry> &registry,
                 const std::string &name, const std::string &help);

  static prometheus::Gauge &
  create_gauge(const std::shared_ptr<prometheus::Registry> &registry,
               const std::string &name, const std::string &help);

  static prometheus::Histogram &
  create_histogram(const std::shared_ptr<prometheus::Registry> &registry,
                   const std::string &name, const std::string &help,
                   const std::vector<double> &buckets);

  // Labeled families: series are created lazily via Family::Add(labels) which
  // deduplicates by label set and is internally synchronized, so recording
  // code can call Add() on every request without caching. Bucket bounds are
  // supplied per Add call (the family does not remember them).
  static prometheus::Family<prometheus::Counter> &
  create_counter_family(const std::shared_ptr<prometheus::Registry> &registry,
                        const std::string &name, const std::string &help);

  static prometheus::Family<prometheus::Gauge> &
  create_gauge_family(const std::shared_ptr<prometheus::Registry> &registry,
                      const std::string &name, const std::string &help);

  static prometheus::Family<prometheus::Histogram> &
  create_histogram_family(const std::shared_ptr<prometheus::Registry> &registry,
                          const std::string &name, const std::string &help);

  template <std::size_t N>
  static prometheus::Histogram &
  create_histogram(const std::shared_ptr<prometheus::Registry> &registry,
                   const std::string &name, const std::string &help,
                   const std::array<double, N> &buckets) {
    return create_histogram(
        registry, name, help,
        std::vector<double>(buckets.begin(), buckets.end()));
  }

private:
  // Shared builder chains: counter/gauge singles and every labeled family use
  // the same prometheus-cpp calls; only the histogram single differs (it adds
  // with bucket bounds).
  template <typename Metric>
  static Metric &
  create_single_metric(const std::shared_ptr<prometheus::Registry> &registry,
                       const std::string &name, const std::string &help) {
    return metrics_detail::Builder<Metric>::of().Name(name).Help(help).Register(
        *registry).Add({});
  }

  template <typename Metric>
  static prometheus::Family<Metric> &
  create_metric_family(const std::shared_ptr<prometheus::Registry> &registry,
                       const std::string &name, const std::string &help) {
    return metrics_detail::Builder<Metric>::of().Name(name).Help(help).Register(
        *registry);
  }
};

// Records a DB-gateway request counter labelled by db/type/status. Shared by the
// proxy (route_db_request) and the worker (process_db_query_from_nats) so the
// label set stays in one place instead of being hand-written at every exit path.
inline void record_db_request_metrics(
    prometheus::Family<prometheus::Counter> &total_family,
    const std::string &db, const std::string &type, int status) {
  total_family
      .Add({{"db", db},
            {"type", type.empty() ? "unknown" : type},
            {"status", std::to_string(status)}})
      .Increment();
}

// Observes a DB-gateway duration histogram (seconds) for the given db label.
// The family does not remember bucket bounds, hence latency_buckets_ms_to_10s().
inline void observe_db_request_duration(
    prometheus::Family<prometheus::Histogram> &duration_family,
    const std::string &db, uint64_t start_us, uint64_t end_us) {
  duration_family.Add({{"db", db}}, latency_buckets_ms_to_10s())
      .Observe(TimeUtils::duration_seconds(start_us, end_us));
}

// Short-hand for the registration calls: a mode registers dozens of metrics
// on its own registry, and repeating the registry argument at every call makes
// the aggregate initialisers of the metric structs hard to read. One registrar
// per registry keeps only the name and the help text in the call.
class MetricRegistrar {
public:
  explicit MetricRegistrar(std::shared_ptr<prometheus::Registry> registry)
      : m_registry(std::move(registry)) {}

  [[nodiscard]] prometheus::Counter &
  counter(const std::string &name, const std::string &help) const {
    return MetricsManager::create_counter(m_registry, name, help);
  }

  [[nodiscard]] prometheus::Gauge &
  gauge(const std::string &name, const std::string &help) const {
    return MetricsManager::create_gauge(m_registry, name, help);
  }

  [[nodiscard]] prometheus::Histogram &
  histogram(const std::string &name, const std::string &help,
            const std::vector<double> &buckets) const {
    return MetricsManager::create_histogram(m_registry, name, help, buckets);
  }

  template <std::size_t N>
  [[nodiscard]] prometheus::Histogram &
  histogram(const std::string &name, const std::string &help,
            const std::array<double, N> &buckets) const {
    return MetricsManager::create_histogram(m_registry, name, help, buckets);
  }

  [[nodiscard]] prometheus::Family<prometheus::Counter> &
  counter_family(const std::string &name, const std::string &help) const {
    return MetricsManager::create_counter_family(m_registry, name, help);
  }

  [[nodiscard]] prometheus::Family<prometheus::Gauge> &
  gauge_family(const std::string &name, const std::string &help) const {
    return MetricsManager::create_gauge_family(m_registry, name, help);
  }

  [[nodiscard]] prometheus::Family<prometheus::Histogram> &
  histogram_family(const std::string &name, const std::string &help) const {
    return MetricsManager::create_histogram_family(m_registry, name, help);
  }

private:
  std::shared_ptr<prometheus::Registry> m_registry;
};

// Value of one labelled series of a counter family; 0 when the series does not
// exist (a labelled family is exported only after its first Add()).
inline double labeled_counter_value(
    const prometheus::Family<prometheus::Counter> &family,
    const std::string &label_name, const std::string &label_value) {
  for (const auto &metric_family : family.Collect()) {
    for (const auto &metric : metric_family.metric) {
      for (const auto &label : metric.label) {
        if (label.name == label_name && label.value == label_value) {
          return metric.counter.value;
        }
      }
    }
  }
  return 0.0;
}

// Sum over all the series of a counter family.
inline double family_counter_total(
    const prometheus::Family<prometheus::Counter> &family) {
  double total = 0.0;
  for (const auto &metric_family : family.Collect()) {
    for (const auto &metric : metric_family.metric) {
      total += metric.counter.value;
    }
  }
  return total;
}

#endif // METRICS_MANAGER_HPP
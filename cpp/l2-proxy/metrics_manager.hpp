#ifndef METRICS_MANAGER_HPP
#define METRICS_MANAGER_HPP

#include "time_utils.hpp"
#include <array>
#include <memory>
#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string>
#include <vector>

namespace histogram_buckets {
constexpr std::array<double, 11> g_k_latency_ms_to_5s = {
    0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0};
constexpr std::array<double, 12> g_k_latency_ms_to_10s = {
    0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
constexpr std::array<double, 11> g_k_latency_5ms_to_10s = {
    0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
constexpr std::array<double, 10> g_k_size_100b_to_5mb = {
    100, 500, 1000, 5000, 10000, 50000, 100000, 500000, 1000000, 5000000};
} // namespace histogram_buckets

// Histogram families need the bucket bounds on every Family::Add call (the
// family itself does not remember them), so labeled call sites pass this
// helper instead of rebuilding the vector inline.
inline std::vector<double> latency_buckets_ms_to_10s() {
  return {histogram_buckets::g_k_latency_ms_to_10s.begin(),
          histogram_buckets::g_k_latency_ms_to_10s.end()};
}

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
  // code can call Add() on every request without caching.
  static prometheus::Family<prometheus::Counter> &
  create_counter_family(const std::shared_ptr<prometheus::Registry> &registry,
                        const std::string &name, const std::string &help);

  static prometheus::Family<prometheus::Gauge> &
  create_gauge_family(const std::shared_ptr<prometheus::Registry> &registry,
                      const std::string &name, const std::string &help);

  static prometheus::Family<prometheus::Histogram> &
  create_histogram_family(const std::shared_ptr<prometheus::Registry> &registry,
                          const std::string &name, const std::string &help,
                          const std::vector<double> &buckets);

  template <std::size_t N>
  static prometheus::Family<prometheus::Histogram> &
  create_histogram_family(const std::shared_ptr<prometheus::Registry> &registry,
                          const std::string &name, const std::string &help,
                          const std::array<double, N> &buckets) {
    return create_histogram_family(
        registry, name, help,
        std::vector<double>(buckets.begin(), buckets.end()));
  }

  template <std::size_t N>
  static prometheus::Histogram &
  create_histogram(const std::shared_ptr<prometheus::Registry> &registry,
                   const std::string &name, const std::string &help,
                   const std::array<double, N> &buckets) {
    return create_histogram(
        registry, name, help,
        std::vector<double>(buckets.begin(), buckets.end()));
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

#endif // METRICS_MANAGER_HPP
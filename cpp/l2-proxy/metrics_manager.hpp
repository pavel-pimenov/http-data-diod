#ifndef METRICS_MANAGER_HPP
#define METRICS_MANAGER_HPP

#include <array>
#include <memory>
#include <prometheus/counter.h>
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

#endif // METRICS_MANAGER_HPP
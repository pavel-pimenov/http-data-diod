#ifndef LABELED_HISTOGRAM_COLLECTOR_HPP
#define LABELED_HISTOGRAM_COLLECTOR_HPP

#include "labeled_entries_utils.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <prometheus/collectable.h>
#include <string>
#include <unordered_map>
#include <vector>

// Generic Prometheus collectable that exposes a histogram with a single
// dynamic label (e.g. latency per "client_id"). Mirrors
// LabeledCounterCollector: one instance per label, entries are fed via
// observe() and are evicted by TTL / max_entries exactly like the counters,
// so a label value that stops being observed vanishes from the export.
//
// Rendering a histogram per label is impossible with a static prometheus-cpp
// Family (no dynamic labels, no Family::Remove), hence this snapshot-based
// approach: on every scrape the current label set is exported from the
// recorded entries.
class LabeledHistogramCollector : public prometheus::Collectable {
public:
  // bucket_bounds: increasing upper bounds of the histogram buckets. Values
  // greater than the last bound fall into the implicit +Inf bucket and only
  // contribute to sample_count/sample_sum.
  LabeledHistogramCollector(std::string label_name, std::string metric_name,
                            std::string metric_help,
                            std::vector<double> bucket_bounds,
                            uint64_t ttl_seconds = 300,
                            size_t max_entries = 10000);

  void observe(const std::string &label_value, double value);

  std::vector<prometheus::MetricFamily> Collect() const override;

private:
  struct Entry {
    std::vector<uint64_t> m_bucket_deltas;
    double m_sum = 0.0;
    uint64_t m_count = 0;
    std::chrono::steady_clock::time_point m_last_seen;
  };

  const std::string m_label_name;
  const std::string m_metric_name;
  const std::string m_metric_help;
  const std::vector<double> m_bucket_bounds;
  const uint64_t m_ttl_seconds;
  const size_t m_max_entries;

  mutable std::mutex m_mutex;
  mutable std::unordered_map<std::string, Entry> m_entries;
};

#endif // LABELED_HISTOGRAM_COLLECTOR_HPP

#include "labeled_histogram_collector.hpp"
#include "labeled_entries_utils.hpp"
#include <algorithm>
#include <prometheus/client_metric.h>
#include <prometheus/metric_family.h>
#include <prometheus/metric_type.h>

LabeledHistogramCollector::LabeledHistogramCollector(
    std::string label_name, std::string metric_name, std::string metric_help,
    std::vector<double> bucket_bounds, uint64_t ttl_seconds, size_t max_entries)
    : m_label_name(std::move(label_name)),
      m_metric_name(std::move(metric_name)),
      m_metric_help(std::move(metric_help)),
      m_bucket_bounds(std::move(bucket_bounds)), m_ttl_seconds(ttl_seconds),
      m_max_entries(max_entries) {}

void LabeledHistogramCollector::observe(const std::string &label_value,
                                        double value) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto &entry = m_entries[label_value];
  entry.m_sum += value;
  ++entry.m_count;
  // Prometheus bucket semantics: bucket i counts observations <= bound_i, so
  // an observation lands in the first bucket whose upper bound is >= value.
  const auto it =
      std::lower_bound(m_bucket_bounds.begin(), m_bucket_bounds.end(), value);
  if (it != m_bucket_bounds.end()) {
    const size_t idx = static_cast<size_t>(it - m_bucket_bounds.begin());
    if (entry.m_bucket_deltas.empty()) {
      entry.m_bucket_deltas.assign(m_bucket_bounds.size(), 0);
    }
    ++entry.m_bucket_deltas[idx];
  }
  entry.m_last_seen = std::chrono::steady_clock::now();
}

std::vector<prometheus::MetricFamily>
LabeledHistogramCollector::Collect() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  evict_stale_and_trim(m_entries, m_ttl_seconds, m_max_entries);

  std::vector<prometheus::MetricFamily> result;
  result.reserve(1);

  prometheus::MetricFamily family;
  family.name = m_metric_name;
  family.help = m_metric_help;
  family.type = prometheus::MetricType::Histogram;

  for (const auto &[label_value, entry] : m_entries) {
    auto metric = make_labeled_metric(m_label_name, label_value);
    metric.histogram.sample_count = entry.m_count;
    metric.histogram.sample_sum = entry.m_sum;

    uint64_t cumulative = 0;
    for (size_t i = 0; i < m_bucket_bounds.size(); ++i) {
      cumulative +=
          entry.m_bucket_deltas.empty() ? 0 : entry.m_bucket_deltas[i];
      prometheus::ClientMetric::Bucket bucket;
      bucket.cumulative_count = cumulative;
      bucket.upper_bound = m_bucket_bounds[i];
      metric.histogram.bucket.push_back(bucket);
    }
    family.metric.emplace_back(std::move(metric));
  }

  result.emplace_back(std::move(family));
  return result;
}

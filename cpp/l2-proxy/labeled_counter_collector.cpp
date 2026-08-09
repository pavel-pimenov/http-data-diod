#include "labeled_counter_collector.hpp"
#include "labeled_entries_utils.hpp"
#include <algorithm>
#include <prometheus/client_metric.h>
#include <prometheus/metric_family.h>
#include <prometheus/metric_type.h>

LabeledCounterCollector::LabeledCounterCollector(
    std::string label_name, std::string requests_name,
    std::string requests_help, std::string rejected_name,
    std::string rejected_help, StatsProvider stats_provider,
    uint64_t ttl_seconds, size_t max_entries)
    : m_label_name(std::move(label_name)),
      m_requests_name(std::move(requests_name)),
      m_requests_help(std::move(requests_help)),
      m_rejected_name(std::move(rejected_name)),
      m_rejected_help(std::move(rejected_help)),
      m_stats_provider(std::move(stats_provider)), m_ttl_seconds(ttl_seconds),
      m_max_entries(max_entries) {}

template <uint64_t LabeledCounterCollector::Stats::*Member>
void LabeledCounterCollector::record_impl(const std::string &label_value) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto &entry = m_entries[label_value];
  entry.m_stats.*Member += 1;
  entry.m_last_seen = std::chrono::steady_clock::now();
}

void LabeledCounterCollector::record_request(const std::string &label_value) {
  record_impl<&Stats::m_requests>(label_value);
}

void LabeledCounterCollector::record_rejection(const std::string &label_value) {
  record_impl<&Stats::m_rejected>(label_value);
}

std::vector<prometheus::MetricFamily> LabeledCounterCollector::Collect() const {
  // Snapshot source: replace the recorded entries with the current set from
  // the provider (e.g. IPs tracked by the rate limiter). The provider is
  // called outside the mutex to avoid holding it while the limiter's own
  // mutex is taken.
  if (m_stats_provider) {
    const auto provided = m_stats_provider();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    const auto now = std::chrono::steady_clock::now();
    for (const auto &[label_value, stats] : provided) {
      m_entries[label_value] = Entry{stats, now};
    }
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  evict_stale_and_trim(m_entries, m_ttl_seconds, m_max_entries);

  std::vector<prometheus::MetricFamily> result;
  result.reserve(2);

  prometheus::MetricFamily requests;
  requests.name = m_requests_name;
  requests.help = m_requests_help;
  requests.type = prometheus::MetricType::Counter;

  prometheus::MetricFamily rejected;
  rejected.name = m_rejected_name;
  rejected.help = m_rejected_help;
  rejected.type = prometheus::MetricType::Counter;

  for (const auto &[label_value, entry] : m_entries) {
    if (entry.m_stats.m_requests > 0) {
      auto metric = make_labeled_metric(m_label_name, label_value);
      metric.counter.value = static_cast<double>(entry.m_stats.m_requests);
      requests.metric.emplace_back(std::move(metric));
    }
    if (entry.m_stats.m_rejected > 0) {
      auto metric = make_labeled_metric(m_label_name, label_value);
      metric.counter.value = static_cast<double>(entry.m_stats.m_rejected);
      rejected.metric.emplace_back(std::move(metric));
    }
  }

  // Skip families that have no series: an empty family pollutes /metrics and
  // Grafana treats an absent family the same as an empty one ("No data").
  if (!requests.metric.empty()) {
    result.emplace_back(std::move(requests));
  }
  if (!rejected.metric.empty()) {
    result.emplace_back(std::move(rejected));
  }
  return result;
}
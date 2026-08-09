#ifndef LABELED_ENTRIES_UTILS_HPP
#define LABELED_ENTRIES_UTILS_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <prometheus/client_metric.h>
#include <string>
#include <unordered_map>
#include <vector>

// Builds a ClientMetric pre-populated with a single label. Shared by the
// labeled collectors, each of which previously hand-assembled the identical
// label block.
inline prometheus::ClientMetric
make_labeled_metric(const std::string &label_name,
                    const std::string &label_value) {
  prometheus::ClientMetric metric;
  prometheus::ClientMetric::Label label;
  label.name = label_name;
  label.value = label_value;
  metric.label.emplace_back(std::move(label));
  return metric;
}

// Shared TTL + LRU eviction for the labeled metric collectors
// (LabeledCounterCollector / LabeledHistogramCollector). Both maintain an
// unordered_map<string, Entry> whose Entry has a steady_clock m_last_seen
// member; both want the same policy: drop entries idle for longer than
// ttl_seconds, then, when the map still exceeds max_entries, trim to the cap
// keeping the most recently active label values.
template <typename Entry>
void evict_stale_and_trim(std::unordered_map<std::string, Entry> &entries,
                          uint64_t ttl_seconds, size_t max_entries) {
  if (ttl_seconds > 0) {
    const auto now = std::chrono::steady_clock::now();
    const auto ttl = std::chrono::seconds(ttl_seconds);
    for (auto it = entries.begin(); it != entries.end();) {
      if (now - it->second.m_last_seen > ttl) {
        it = entries.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (entries.size() <= max_entries) {
    return;
  }
  std::vector<const std::string *> oldest;
  oldest.reserve(entries.size());
  for (const auto &kv : entries) {
    oldest.push_back(&kv.first);
  }
  std::sort(oldest.begin(), oldest.end(),
            [&entries](const std::string *lhs, const std::string *rhs) {
              return entries.at(*lhs).m_last_seen <
                     entries.at(*rhs).m_last_seen;
            });
  const size_t to_evict = entries.size() - max_entries;
  for (size_t i = 0; i < to_evict; ++i) {
    entries.erase(*oldest[i]);
  }
}

#endif // LABELED_ENTRIES_UTILS_HPP

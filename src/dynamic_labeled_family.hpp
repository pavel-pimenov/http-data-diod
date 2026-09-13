#ifndef DYNAMIC_LABELED_FAMILY_HPP
#define DYNAMIC_LABELED_FAMILY_HPP

#include <prometheus/collectable.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/labels.h>
#include <prometheus/metric_family.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Replaces the hand-rolled LabeledCounterCollector / LabeledHistogramCollector
// (and the shared labeled_entries_utils) with a wrapper around a real
// prometheus-cpp 1.2.4 Family<T>. Now that Family::Remove exists, series can be
// added/removed dynamically instead of re-assembling MetricFamily by hand, so
// the native family does the (correct) rendering and the wrapper only manages
// label-value lifetimes.
//
// One instance owns one or more metric families (e.g. the requests_total and
// rejected_total counters that share the same dynamic label values), keeping a
// single label set and a single TTL/LRU index in sync across all series that
// share a label value.
//
// Data feeding mirrors the previous collectors:
//  - direct recording via get(label_value, series_index): the caller
//    increments/observes the returned child (Counter/Histogram only);
//  - snapshot provider (Gauge only): on every Collect() the provided label set
//    replaces the exported series via Gauge::Set, so a value that stops being
//    reported disappears immediately.
template <typename T>
class DynamicLabeledFamily : public prometheus::Collectable {
public:
  struct Series {
    std::string m_name;
    std::string m_help;
  };

  // Returns {label_value, per-series values} for every label to export. Only
  // valid with T = prometheus::Gauge (absolute values set via Gauge::Set).
  using Provider =
      std::function<std::vector<std::pair<std::string, std::vector<double>>>()>;

  // label_name: the single dynamic label shared by all series.
  // series: metric name/help for each series sharing the label set.
  // provider: optional snapshot provider (Gauge only).
  // ttl_seconds: drop label values idle longer than this on the next Collect()
  //   (0 disables TTL).
  // max_entries: hard cap on tracked label values; oldest evicted first.
  // histogram_buckets: required bucket boundaries when T = Histogram.
  DynamicLabeledFamily(std::string label_name, std::vector<Series> series,
                       Provider provider = {}, uint64_t ttl_seconds = 300,
                       size_t max_entries = 10000,
                       std::vector<double> histogram_buckets = {})
      : m_label_name(std::move(label_name)), m_series(std::move(series)),
        m_provider(std::move(provider)), m_ttl_seconds(ttl_seconds),
        m_max_entries(max_entries),
        m_histogram_buckets(std::move(histogram_buckets)) {
    m_families.reserve(m_series.size());
    for (const auto& s : m_series) {
      m_families.push_back(std::make_shared<prometheus::Family<T>>(
          s.m_name, s.m_help, prometheus::Labels{}));
    }
  }

  // Returns the child metric for the given label value and series index,
  // creating it on first use and refreshing its last-seen timestamp.
  T* get(const std::string& label_value, size_t series_index) {
    std::lock_guard lock(m_mutex);
    ensure_child(label_value);
    m_last_seen[label_value] = std::chrono::steady_clock::now();
    return m_children.at(label_value)[series_index];
  }

  std::vector<prometheus::MetricFamily> Collect() const override {
    std::lock_guard lock(m_mutex);
    if constexpr (std::is_same_v<T, prometheus::Gauge>) {
      if (m_provider) {
        replace_from_provider();
      } else {
        evict_stale_and_trim();
      }
    } else {
      evict_stale_and_trim();
    }
    std::vector<prometheus::MetricFamily> result;
    for (const auto& family : m_families) {
      auto collected = family->Collect();
      if (!collected.empty()) {
        result.insert(result.end(), std::make_move_iterator(collected.begin()),
                      std::make_move_iterator(collected.end()));
      }
    }
    return result;
  }

private:
  const std::string m_label_name;
  const std::vector<Series> m_series;
  const Provider m_provider;
  const uint64_t m_ttl_seconds;
  const size_t m_max_entries;
  const std::vector<double> m_histogram_buckets;

  mutable std::mutex m_mutex;
  mutable std::vector<std::shared_ptr<prometheus::Family<T>>> m_families;
  mutable std::unordered_map<std::string, std::vector<T*>> m_children;
  mutable std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      m_last_seen;

  void ensure_child(const std::string& label_value) const {
    if (m_children.count(label_value) != 0) {
      return;
    }
    const prometheus::Labels labels{{m_label_name, label_value}};
    std::vector<T*> series;
    series.reserve(m_families.size());
    for (const auto& family : m_families) {
      if constexpr (std::is_same_v<T, prometheus::Histogram>) {
        series.push_back(&family->Add(labels, m_histogram_buckets));
      } else {
        series.push_back(&family->Add(labels));
      }
    }
    m_children.emplace(label_value, std::move(series));
  }

  void remove_label(const std::string& label_value) const {
    const auto it = m_children.find(label_value);
    if (it == m_children.end()) {
      return;
    }
    for (size_t i = 0; i < it->second.size(); ++i) {
      m_families[i]->Remove(it->second[i]);
    }
    m_children.erase(it);
  }

  void evict_stale_and_trim() const {
    if (m_ttl_seconds > 0) {
      const auto now = std::chrono::steady_clock::now();
      const auto ttl = std::chrono::seconds(m_ttl_seconds);
      for (auto it = m_last_seen.begin(); it != m_last_seen.end();) {
        if (now - it->second > ttl) {
          const auto label = it->first;
          remove_label(label);
          it = m_last_seen.erase(it);
        } else {
          ++it;
        }
      }
    }

    if (m_children.size() <= m_max_entries) {
      return;
    }
    std::vector<const std::string*> oldest;
    oldest.reserve(m_children.size());
    for (const auto& kv : m_children) {
      oldest.push_back(&kv.first);
    }
    const size_t to_evict = m_children.size() - m_max_entries;
    // nth_element places the to_evict oldest (by last activity) in the front.
    std::nth_element(oldest.begin(), oldest.begin() + to_evict, oldest.end(),
                     [this](const std::string* lhs, const std::string* rhs) {
                       return m_last_seen.at(*lhs) < m_last_seen.at(*rhs);
                     });
    for (size_t i = 0; i < to_evict; ++i) {
      // Copy the label before removing it: remove_label erases the map key,
      // which would invalidate the dangling oldest[i] pointer.
      const auto label = *oldest[i];
      m_last_seen.erase(label);
      remove_label(label);
    }
  }

  void replace_from_provider() const {
    static_assert(std::is_same_v<T, prometheus::Gauge>,
                  "snapshot provider requires a Gauge family");
    std::unordered_set<std::string> present;
    const auto provided = m_provider();
    for (const auto& [label_value, values] : provided) {
      present.insert(label_value);
      ensure_child(label_value);
      m_last_seen[label_value] = std::chrono::steady_clock::now();
      const auto& series = m_children.at(label_value);
      for (size_t i = 0; i < series.size(); ++i) {
        static_cast<prometheus::Gauge*>(series[i])->Set(values[i]);
      }
    }
    for (auto it = m_children.begin(); it != m_children.end();) {
      if (present.count(it->first) == 0) {
        // Erase last_seen first: remove_label erases the map key, invalidating
        // it->first afterwards.
        auto label = it->first;
        m_last_seen.erase(label);
        remove_label(label);
        it = m_children.begin();
      } else {
        ++it;
      }
    }
  }
};

#endif // DYNAMIC_LABELED_FAMILY_HPP

#ifndef LABELED_COUNTER_COLLECTOR_HPP
#define LABELED_COUNTER_COLLECTOR_HPP

#include "labeled_entries_utils.hpp"
#include <chrono>
#include <functional>
#include <mutex>
#include <prometheus/collectable.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Generic Prometheus collectable that exposes request/rejection counters with a
// single dynamic label (e.g. "ip" or "client_id"). One instance per label:
// future per-header/per-parameter distributions just add another instance
// instead of a new collector class.
//
// Two ways to feed it:
//  - direct recording: record_request()/record_rejection() increment counters
//    owned by this instance (per-client-id case);
//  - snapshot provider: a std::function invoked on every scrape that replaces
//    the whole entry set (per-IP case, where the counters live in the
//    PerIPRateLimiter and IPs come and go).
//
// prometheus-cpp 1.0.2 has no Family::Remove, so label values that
// appear/disappear cannot be cleaned up on a regular Family. Instead this
// collector snapshots the current label-value set on every scrape (via the
// provider or the recorded entries), so stale series vanish as soon as a value
// stops being reported.
class LabeledCounterCollector : public prometheus::Collectable {
public:
  struct Stats {
    uint64_t m_requests = 0;
    uint64_t m_rejected = 0;
  };

  // Returns a fresh {label_value, counters} snapshot for label values whose
  // counters are owned elsewhere. Invoked once per scrape when set.
  using StatsProvider =
      std::function<std::vector<std::pair<std::string, Stats>>()>;

  // ttl_seconds: label values untouched for this long are dropped on the next
  // scrape, so a vanished client/header value stops being exported. For the
  // snapshot-provider path the provider already owns entry lifetimes and the
  // TTL only acts as a safety net.
  // max_entries: hard cap on the number of tracked label values; the oldest
  // (by last activity) entries are evicted first, bounding memory usage under
  // a flood of unique header values.
  LabeledCounterCollector(std::string label_name, std::string requests_name,
                          std::string requests_help, std::string rejected_name,
                          std::string rejected_help,
                          StatsProvider stats_provider = {},
                          uint64_t ttl_seconds = 300,
                          size_t max_entries = 10000);

  void record_request(const std::string &label_value);
  void record_rejection(const std::string &label_value);

  std::vector<prometheus::MetricFamily> Collect() const override;

private:
  // Shared body of record_request/record_rejection: increments the given
  // Stats member for the label value and refreshes its last-seen timestamp.
  template <uint64_t Stats::*Member>
  void record_impl(const std::string &label_value);

  struct Entry {
    Stats m_stats;
    std::chrono::steady_clock::time_point m_last_seen;
  };

  const std::string m_label_name;
  const std::string m_requests_name;
  const std::string m_requests_help;
  const std::string m_rejected_name;
  const std::string m_rejected_help;
  const StatsProvider m_stats_provider;
  const uint64_t m_ttl_seconds;
  const size_t m_max_entries;

  mutable std::mutex m_mutex;
  mutable std::unordered_map<std::string, Entry> m_entries;
};

#endif // LABELED_COUNTER_COLLECTOR_HPP

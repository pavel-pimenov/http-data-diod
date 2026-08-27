#ifndef DUPLICATE_DETECTOR_HPP
#define DUPLICATE_DETECTOR_HPP

#include "nlohmann/json.hpp"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#if __has_include(<flat_set>)
#include <flat_set>
#endif

// Detects duplicate POST requests from clients in the proxy.
//
// A "duplicate" is the same request body (keyed by its SHA-256 digest)
// delivered more than once within a TTL window. The detector keeps a bounded
// set of recently-seen bodies, records how many times each was seen and by
// which client ids, and classifies each duplicated body:
//   - "same_client": all deliveries came from a single client id;
//   - "cross_client": the same body arrived from different client ids.
// A short body sample (<= m_max_body_bytes, default 500) is stored so the
// report endpoint can show the payload. The report serves the top
// m_top_n duplicates by occurrence count.
//
// Thread-safety: the proxy handles requests on many threads concurrently, so
// all state is guarded by a mutex.
class DuplicateDetector {
public:
  struct Options {
    bool m_enabled = true;
    size_t m_top_n = 100;          // max duplicates in the report
    size_t m_max_entries = 1000;   // bound on tracked distinct bodies
    uint64_t m_ttl_ms = 60000;     // inactivity window for a tracked body
    size_t m_max_body_bytes = 500; // store body sample only if <= this size
  };

  // Default-constructed detector keeps the default Options (see .cpp; the
  // default argument is written out-of-line because Options' default member
  // initializers are not usable in a default argument inside this class body).
  DuplicateDetector();
  explicit DuplicateDetector(const Options &options);

  // Records one request body. Returns true when this delivery makes the body
  // a duplicate (seen at least twice within the TTL window).
  bool record(std::string_view client_id, std::string_view body_hash,
              std::string_view body);

  // How many distinct bodies were seen more than once (for metrics).
  size_t duplicate_bodies() const;

  // JSON report: {enabled, duplicate_bodies, duplicate_occurrences,
  // by_type: {same_client, cross_client}, top: [...]}.
  nlohmann::json report() const;

private:
#if __has_include(<flat_set>) && defined(__cpp_lib_flat_set)
  using ClientIdSet = std::flat_set<std::string>;
#else
  using ClientIdSet = std::set<std::string>;
#endif
  struct Entry {
    std::string m_body; // body sample (empty if larger than m_max_body_bytes)
    ClientIdSet m_client_ids;
    uint64_t m_first_seen_ms = 0;
    uint64_t m_last_seen_ms = 0;
    uint64_t m_count = 0;
  };

  void evict_expired_locked(uint64_t now_ms);
  // Evicts the entry with the smallest occurrence count (oldest among ties)
  // so the most interesting duplicates survive when the map is full.
  void evict_lowest_count_locked();

  Options m_options;
  mutable std::mutex m_mutex;
  std::unordered_map<std::string, Entry> m_entries; // key: sha256 hex
};

#endif // DUPLICATE_DETECTOR_HPP

#ifndef DEDUP_CACHE_HPP
#define DEDUP_CACHE_HPP

#include "time_utils.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// Bounded cache of recently produced responses keyed by request_id.
//
// The proxy re-sends a NATS request/reply when it does not receive an answer
// before its deadline (e.g. the reply was lost while NATS was reconnecting).
// Without a cache the worker would process the same request twice, causing a
// duplicate side-effect on the L2 server. Storing the produced response and
// returning it on a repeated delivery gives at-most-once L2 side effects while
// still letting the proxy complete the retried request.
//
// Thread-safety: worker threads process NATS messages concurrently, so all
// access is guarded by a mutex. The TTL must cover the proxy retry window
// (REQUEST_TIMEOUT_SECONDS, default 30s); the default is 60s. Parameters are
// configured from env (DEDUP_ENABLED / DEDUP_MAX_ENTRIES / DEDUP_TTL_MS).
// When disabled the cache is a no-op: find() never hits and store() is
// skipped, so a re-delivered request is processed again (at-least-once L2
// side effects instead of at-most-once).
class DedupCache {
public:
  explicit DedupCache(bool enabled = true, size_t max_entries = 4096,
                      uint64_t ttl_ms = 60000)
      : m_enabled(enabled), m_max_entries(max_entries), m_ttl_ms(ttl_ms) {}

  // Returns the cached response for request_id if it was produced within the
  // TTL window, nullopt otherwise. Does not extend the entry lifetime.
  std::optional<std::string> find(std::string_view request_id) const {
    if (!m_enabled) {
      return std::nullopt;
    }
    const uint64_t now_ms = now_ms_since_epoch();
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(std::string(request_id));
    if (it == m_entries.end()) {
      return std::nullopt;
    }
    if (it->second.m_expires_at_ms <= now_ms) {
      return std::nullopt;
    }
    return it->second.m_response;
  }

  // Stores (or refreshes) the response for request_id. Opportunistically
  // evicts expired entries and, when the cache is full, the oldest ones.
  void store(std::string_view request_id, std::string response_json) {
    if (!m_enabled) {
      return;
    }
    const uint64_t now_ms = now_ms_since_epoch();
    std::lock_guard<std::mutex> lock(m_mutex);
    evict_expired_locked(now_ms);

    const std::string key(request_id);
    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
      it->second.m_response = std::move(response_json);
      it->second.m_expires_at_ms = now_ms + m_ttl_ms;
      return;
    }

    while (m_entries.size() >= m_max_entries) {
      evict_oldest_locked();
    }
    m_entries.emplace(key, Entry{std::move(response_json), now_ms + m_ttl_ms});
    m_order.push_back(key);
  }

private:
  struct Entry {
    std::string m_response;
    uint64_t m_expires_at_ms;
  };

  static uint64_t now_ms_since_epoch() { return TimeUtils::steady_ms(); }

  void evict_expired_locked(uint64_t now_ms) {
    while (!m_order.empty()) {
      const auto it = m_entries.find(m_order.front());
      if (it == m_entries.end() || it->second.m_expires_at_ms > now_ms) {
        break;
      }
      m_entries.erase(it);
      m_order.pop_front();
    }
  }

  void evict_oldest_locked() {
    if (m_order.empty()) {
      return;
    }
    m_entries.erase(m_order.front());
    m_order.pop_front();
  }

  mutable std::mutex m_mutex;
  std::unordered_map<std::string, Entry> m_entries;
  std::deque<std::string> m_order; // request_ids in insertion order
  bool m_enabled;
  size_t m_max_entries;
  uint64_t m_ttl_ms;
};

#endif // DEDUP_CACHE_HPP

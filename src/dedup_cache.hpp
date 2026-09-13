#ifndef DEDUP_CACHE_HPP
#define DEDUP_CACHE_HPP

#include "time_utils.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

consteval std::size_t dedup_default_max() { return 4096; }
consteval std::uint64_t dedup_default_ttl() { return 60000; }

class DedupCache {
public:
  explicit DedupCache(bool enabled = true, size_t max_entries = dedup_default_max(),
                      uint64_t ttl_ms = dedup_default_ttl())
      : m_enabled(enabled), m_max_entries(max_entries), m_ttl_ms(ttl_ms) {}

  [[nodiscard]] std::optional<std::string>
  find(std::string_view request_id) const {
    if (!m_enabled) {
      return std::nullopt;
    }
    const uint64_t now_ms = now_ms_since_epoch();
    std::lock_guard lock(m_mutex);
    const auto it = m_entries.find(std::string(request_id));
    if (it == m_entries.end()) {
      return std::nullopt;
    }
    if (it->second.m_expires_at_ms <= now_ms) {
      // m_entries / m_order are mutable, so erasing from a const method is safe
      // (still guarded by the mutex).
      m_order.erase(it->second.m_order_iter);
      m_entries.erase(it);
      return std::nullopt;
    }
    return it->second.m_response;
  }

  void store(std::string_view request_id, std::string response_json) {
    if (!m_enabled) {
      return;
    }
    const uint64_t now_ms = now_ms_since_epoch();
    std::lock_guard lock(m_mutex);
    evict_expired_locked(now_ms);

    const std::string key(request_id);
    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
      it->second.m_response = std::move(response_json);
      it->second.m_expires_at_ms = now_ms + m_ttl_ms;
      // O(1): splice the existing node to the back of the list
      m_order.splice(m_order.end(), m_order, it->second.m_order_iter);
      return;
    }

    while (m_entries.size() >= m_max_entries) {
      evict_oldest_locked();
    }
    m_order.push_back(key);
    auto order_iter = std::prev(m_order.end());
    m_entries.emplace(key, Entry{std::move(response_json), now_ms + m_ttl_ms, order_iter});
  }

private:
  struct Entry {
    std::string m_response;
    uint64_t m_expires_at_ms;
    std::list<std::string>::iterator m_order_iter;
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
  mutable std::unordered_map<std::string, Entry> m_entries;
  mutable std::list<std::string> m_order;
  bool m_enabled;
  size_t m_max_entries;
  uint64_t m_ttl_ms;
};

#endif // DEDUP_CACHE_HPP

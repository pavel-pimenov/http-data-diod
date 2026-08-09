#ifndef RATE_LIMITER_PER_IP_HPP
#define RATE_LIMITER_PER_IP_HPP

#include "logger.hpp"
#include "rate_limiter.hpp"
#include <atomic>
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <unordered_map>

class PerIPRateLimiter {
public:
  // Per-IP counters exposed via LabeledCounterCollector (label "ip")
  struct IPStats {
    uint64_t m_requests = 0;
    uint64_t m_rejected = 0;
  };

private:
  struct IPEntry {
    std::shared_ptr<RateLimiter> m_limiter;
    std::chrono::steady_clock::time_point m_last_seen;
    std::list<std::string>::iterator m_lru_it;
    std::atomic<uint64_t> m_requests{0};
    std::atomic<uint64_t> m_rejected{0};

    IPEntry(std::shared_ptr<RateLimiter> l, std::list<std::string>::iterator it)
        : m_limiter(std::move(l)),
          m_last_seen(std::chrono::steady_clock::now()), m_lru_it(it) {}
  };

  const uint64_t m_max_tokens_per_ip;
  const uint64_t m_refill_tokens_per_second_per_ip;
  const size_t m_max_ips;
  const int m_cleanup_interval_seconds;

  std::unordered_map<std::string, IPEntry> m_ip_entries;
  std::list<std::string> m_lru_list; // front = oldest, back = newest
  mutable std::mutex m_mutex;

  std::atomic<uint64_t> m_total_requests{0};
  std::atomic<uint64_t> m_allowed_requests{0};
  std::atomic<uint64_t> m_rejected_requests{0};
  std::atomic<uint64_t> m_unique_ips{0};
  std::atomic<uint64_t> m_evictions{0};

  std::thread m_cleanup_thread;
  std::atomic<bool> m_running{false};

public:
  PerIPRateLimiter(uint64_t max_tokens_per_ip,
                   uint64_t refill_tokens_per_second_per_ip,
                   size_t max_ips = 10000, int cleanup_interval_seconds = 300)
      : m_max_tokens_per_ip(max_tokens_per_ip),
        m_refill_tokens_per_second_per_ip(refill_tokens_per_second_per_ip),
        m_max_ips(max_ips),
        m_cleanup_interval_seconds(cleanup_interval_seconds) {
    Logger::info("PerIPRateLimiter initialized: max_tokens_per_ip={} "
                 "refill_per_sec={} max_ips={} cleanup_ttl={}s",
                 max_tokens_per_ip, refill_tokens_per_second_per_ip, max_ips,
                 cleanup_interval_seconds);

    start_background_cleanup();
  }

  ~PerIPRateLimiter() { stop_background_cleanup(); }

  PerIPRateLimiter(const PerIPRateLimiter &) = delete;
  PerIPRateLimiter &operator=(const PerIPRateLimiter &) = delete;
  PerIPRateLimiter(PerIPRateLimiter &&) = delete;
  PerIPRateLimiter &operator=(PerIPRateLimiter &&) = delete;

  bool acquire(const std::string &client_ip) {
    m_total_requests.fetch_add(1, std::memory_order_relaxed);

    std::shared_ptr<RateLimiter> limiter = get_or_create_limiter(client_ip);
    if (!limiter) {
      m_rejected_requests.fetch_add(1, std::memory_order_relaxed);
      Logger::warn(
          "PerIPRateLimiter: too many IPs tracked ({} >= max_ips={}), "
          "rejecting request from {}. Consider increasing PER_IP_MAX_IPS "
          "and/or lowering PER_IP_CLEANUP_TTL_SECONDS to free stale entries",
          m_ip_entries.size(), m_max_ips, client_ip);
      return false;
    }

    if (limiter->acquire()) {
      m_allowed_requests.fetch_add(1, std::memory_order_relaxed);
      return true;
    } else {
      m_rejected_requests.fetch_add(1, std::memory_order_relaxed);
      record_rejection(client_ip);
      Logger::debug(
          "PerIPRateLimiter: rate limit exceeded for IP {} (per-IP bucket "
          "empty: max_tokens_per_ip={} burst, refill={}/s). Exceeded sustained "
          "per-IP request rate. If this is legitimate traffic, increase "
          "PER_IP_MAX_TOKENS (burst capacity) or PER_IP_REFILL_RATE "
          "(sustained req/s per IP)",
          client_ip, m_max_tokens_per_ip, m_refill_tokens_per_second_per_ip);
      return false;
    }
  }

  std::shared_ptr<RateLimiter>
  get_or_create_limiter(const std::string &client_ip) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_ip_entries.find(client_ip);
    if (it != m_ip_entries.end()) {
      it->second.m_last_seen = std::chrono::steady_clock::now();
      it->second.m_requests.fetch_add(1, std::memory_order_relaxed);
      // Move to back (most recently used) — O(1)
      m_lru_list.splice(m_lru_list.end(), m_lru_list, it->second.m_lru_it);
      return it->second.m_limiter;
    }

    if (m_ip_entries.size() >= m_max_ips) {
      evict_oldest_ips(1);

      if (m_ip_entries.size() >= m_max_ips) {
        return nullptr;
      }
    }

    m_lru_list.push_back(client_ip);
    const auto lru_it = std::prev(m_lru_list.end());
    const auto limiter = std::make_shared<RateLimiter>(
        m_max_tokens_per_ip, m_refill_tokens_per_second_per_ip);
    // try_emplace builds IPEntry in place: the atomic counter members make
    // IPEntry non-copyable, so a temporary IPEntry(...) cannot be emplaced.
    const auto insert_result =
        m_ip_entries.try_emplace(client_ip, limiter, lru_it);
    m_unique_ips.fetch_add(1, std::memory_order_relaxed);
    insert_result.first->second.m_requests.fetch_add(1,
                                                     std::memory_order_relaxed);

    Logger::debug("PerIPRateLimiter: created limiter for IP {} (total IPs: {})",
                  client_ip, m_ip_entries.size());

    return limiter;
  }

  struct Stats {
    uint64_t m_total_requests;
    uint64_t m_allowed_requests;
    uint64_t m_rejected_requests;
    uint64_t m_unique_ips;
    size_t m_tracked_ips;
    uint64_t m_evictions;
    double m_rejection_rate;
  };

  [[nodiscard]] uint64_t max_tokens_per_ip() const {
    return m_max_tokens_per_ip;
  }

  [[nodiscard]] uint64_t refill_tokens_per_second_per_ip() const {
    return m_refill_tokens_per_second_per_ip;
  }

  Stats get_stats() const {
    uint64_t total = m_total_requests.load();
    uint64_t rejected = m_rejected_requests.load();
    std::lock_guard<std::mutex> lock(m_mutex);
    return Stats{total,
                 m_allowed_requests.load(),
                 rejected,
                 m_unique_ips.load(),
                 m_ip_entries.size(),
                 m_evictions.load(),
                 total > 0 ? static_cast<double>(rejected) / total : 0.0};
  }

  // Snapshot of per-IP counters for the metrics collector. Entries are
  // ordered most-recently-used first (back of the LRU list = newest).
  std::vector<std::pair<std::string, IPStats>> get_per_ip_stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::pair<std::string, IPStats>> result;
    result.reserve(m_ip_entries.size());
    for (const std::string &ip : std::views::reverse(m_lru_list)) {
      const auto entry_it = m_ip_entries.find(ip);
      if (entry_it != m_ip_entries.end()) {
        result.emplace_back(
            ip,
            IPStats{
                entry_it->second.m_requests.load(std::memory_order_relaxed),
                entry_it->second.m_rejected.load(std::memory_order_relaxed)});
      }
    }
    return result;
  }

  size_t cleanup_expired_ips() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return do_cleanup_expired();
  }

private:
  void record_rejection(const std::string &client_ip) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_ip_entries.find(client_ip);
    if (it != m_ip_entries.end()) {
      it->second.m_rejected.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void start_background_cleanup() {
    m_cleanup_thread = std::thread([this]() {
      m_running.store(true, std::memory_order_release);
      Logger::debug("PerIPRateLimiter: background cleanup thread started");
      const int cleanup_every_seconds = m_cleanup_interval_seconds / 2 + 1;
      while (m_running.load(std::memory_order_acquire)) {
        // Sleep in 1-second steps so shutdown (m_running=false) is observed
        // promptly instead of waiting out the whole cleanup interval.
        for (int i = 0; i < cleanup_every_seconds &&
                        m_running.load(std::memory_order_acquire);
             ++i) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!m_running.load(std::memory_order_acquire))
          break;
        cleanup_expired_ips();
      }
      Logger::debug("PerIPRateLimiter: background cleanup thread stopped");
    });
    while (!m_running.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  void stop_background_cleanup() {
    m_running.store(false, std::memory_order_release);
    if (m_cleanup_thread.joinable()) {
      m_cleanup_thread.join();
    }
  }

  size_t do_cleanup_expired() {
    const auto now = std::chrono::steady_clock::now();
    size_t removed = 0;

    for (auto it = m_ip_entries.begin(); it != m_ip_entries.end();) {
      const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                           now - it->second.m_last_seen)
                           .count();

      if (age >= m_cleanup_interval_seconds) {
        m_lru_list.erase(it->second.m_lru_it);
        it = m_ip_entries.erase(it);
        removed++;
      } else {
        ++it;
      }
    }

    if (removed > 0) {
      m_evictions.fetch_add(removed, std::memory_order_relaxed);
      Logger::debug("PerIPRateLimiter: TTL cleanup removed {} expired IPs "
                    "(remaining: {})",
                    removed, m_ip_entries.size());
    }

    return removed;
  }

  void evict_oldest_ips(size_t count) {
    size_t removed = 0;
    for (size_t i = 0; i < count && !m_lru_list.empty(); ++i) {
      const std::string &oldest_ip = m_lru_list.front();
      m_ip_entries.erase(oldest_ip);
      m_lru_list.pop_front();
      removed++;
    }

    if (removed > 0) {
      m_evictions.fetch_add(removed, std::memory_order_relaxed);
      Logger::debug("PerIPRateLimiter: LRU eviction removed {} oldest IPs "
                    "(remaining: {})",
                    removed, m_ip_entries.size());
    }
  }
};

#endif // RATE_LIMITER_PER_IP_HPP

#include "duplicate_detector.hpp"
#include "logger.hpp"
#include "time_utils.hpp"
#include <algorithm>
#include <chrono>
#include <ranges>
#include <utility>
#if __has_include(<print>)
#include <print>
#endif

DuplicateDetector::DuplicateDetector() : DuplicateDetector(Options{}) {}

DuplicateDetector::DuplicateDetector(const Options &options)
    : m_options(options) {}

std::pair<bool, size_t> DuplicateDetector::record(std::string_view client_id,
                                                  std::string_view client_ip,
                                                  std::string_view body_hash,
                                                  std::string_view body) {
  if (!m_options.m_enabled) {
    return {false, 0};
  }
  const uint64_t now_ms = TimeUtils::steady_ms();
  std::lock_guard lock(m_mutex);
  evict_expired_locked(now_ms);

  const std::string key(body_hash);
  auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    if (m_entries.size() >= m_options.m_max_entries) {
      evict_lowest_count_locked();
    }
    Entry entry;
    entry.m_client_ids.insert(std::string(client_id));
    entry.m_first_seen_ms = now_ms;
    entry.m_last_seen_ms = now_ms;
    entry.m_count = 1;
    if (body.size() <= m_options.m_max_body_bytes) {
      entry.m_body = std::string(body);
    }
    m_entries.emplace(key, std::move(entry));
    return {false, 0};
  }

  Entry &entry = it->second;
  entry.m_count += 1;
  entry.m_last_seen_ms = now_ms;
  entry.m_client_ids.insert(std::string(client_id));
  if (entry.m_body.empty() && body.size() <= m_options.m_max_body_bytes) {
    entry.m_body = std::string(body);
  }
  if (entry.m_count < 2) {
    return {false, 0};
  }
  const std::string client_key(client_id);
  auto cit = m_per_client_count.find(client_key);
  if (cit == m_per_client_count.end()) {
    if (m_options.m_per_client_max_entries > 0 &&
        m_per_client_count.size() >= m_options.m_per_client_max_entries) {
      evict_oldest_client_locked();
    }
    cit = m_per_client_count.emplace(client_key, ClientCount{})
              .first;
  }
  ClientCount &client_state = cit->second;
  client_state.m_count += 1;
  client_state.m_last_seen_ms = now_ms;
  const size_t client_count = client_state.m_count;
  if (m_options.m_duplicate_log_threshold > 0 &&
      client_count % m_options.m_duplicate_log_threshold == 0) {
    Logger::warn(
        "Frequent duplicate POSTs from client_id={} client_ip={} total={} "
        "threshold={} body_bytes={}",
        client_id, client_ip, client_count, m_options.m_duplicate_log_threshold,
        body.size());
  }
  return {true, client_count};
}

size_t DuplicateDetector::duplicate_bodies() const {
  std::lock_guard lock(m_mutex);
  return static_cast<size_t>(std::ranges::count_if(
      m_entries, [](const auto &kv) { return kv.second.m_count >= 2; }));
}

size_t DuplicateDetector::per_client_count_size() const {
  std::lock_guard lock(m_mutex);
  return m_per_client_count.size();
}

nlohmann::json DuplicateDetector::report() const {
  std::lock_guard lock(m_mutex);
  nlohmann::json result;
  result["enabled"] = m_options.m_enabled; //-V601 nlohmann::json handles bool

  std::vector<const Entry *> duplicates;
  size_t duplicate_occurrences = 0;
  size_t same_client = 0;
  size_t cross_client = 0;
  // ranges::filter + ranges::to — C++23 сахар вместо ручного цикла
  for (const auto &[hash, entry] : m_entries | std::views::filter([](const auto &kv){ return kv.second.m_count >= 2; })) {
    duplicates.push_back(&entry);
    duplicate_occurrences += entry.m_count - 1;
    if (entry.m_client_ids.size() <= 1) ++same_client; else ++cross_client;
  }

  std::ranges::sort(duplicates,
            [](const Entry *a, const Entry *b) {
              if (a->m_count != b->m_count) return a->m_count > b->m_count;
              return a->m_first_seen_ms < b->m_first_seen_ms;
            });

  result["duplicate_bodies"] = duplicates.size();
  result["duplicate_occurrences"] = duplicate_occurrences;
  result["by_type"] = {{"same_client", same_client},
                       {"cross_client", cross_client}};

  nlohmann::json top = nlohmann::json::array();
  const size_t n = std::min(duplicates.size(), m_options.m_top_n);
  for (size_t i = 0; i < n; ++i) {
    const Entry &entry = *duplicates[i];
    nlohmann::json item;
    item["count"] = entry.m_count;
    item["type"] =
        entry.m_client_ids.size() <= 1 ? "same_client" : "cross_client";
    item["clients"] = nlohmann::json::array();
    for (const auto &client : entry.m_client_ids) {
      item["clients"].push_back(client);
    }
    item["first_seen_ms"] = entry.m_first_seen_ms;
    item["last_seen_ms"] = entry.m_last_seen_ms;
    item["body"] = entry.m_body;
    top.push_back(std::move(item));
  }
  result["top"] = std::move(top);
  return result;
}

void DuplicateDetector::evict_expired_locked(uint64_t now_ms) {
  for (auto it = m_entries.begin(); it != m_entries.end();) {
    if (now_ms - it->second.m_last_seen_ms > m_options.m_ttl_ms) {
      it = m_entries.erase(it);
    } else {
      ++it;
    }
  }
  evict_expired_clients_locked(now_ms);
}

void DuplicateDetector::evict_expired_clients_locked(uint64_t now_ms) {
  if (m_options.m_per_client_ttl_ms == 0) {
    return;
  }
  for (auto it = m_per_client_count.begin(); it != m_per_client_count.end();) {
    if (now_ms - it->second.m_last_seen_ms > m_options.m_per_client_ttl_ms) {
      it = m_per_client_count.erase(it);
    } else {
      ++it;
    }
  }
}

void DuplicateDetector::evict_oldest_client_locked() {
  if (m_per_client_count.empty()) {
    return;
  }
  const auto victim = std::ranges::min_element(
      m_per_client_count, [](const auto &a, const auto &b) {
        return a.second.m_last_seen_ms < b.second.m_last_seen_ms;
      });
  m_per_client_count.erase(victim);
}

void DuplicateDetector::evict_lowest_count_locked() {
  if (m_entries.empty()) {
    return;
  }
  auto victim = std::ranges::min_element(
      m_entries, [](const auto &a, const auto &b) {
        if (a.second.m_count != b.second.m_count) {
          return a.second.m_count < b.second.m_count;
        }
        return a.second.m_first_seen_ms < b.second.m_first_seen_ms;
      });
  m_entries.erase(victim);
}

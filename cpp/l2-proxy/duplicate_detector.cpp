#include "duplicate_detector.hpp"
#include "time_utils.hpp"
#include <algorithm>
#include <chrono>
#include <utility>

DuplicateDetector::DuplicateDetector() : DuplicateDetector(Options{}) {}

DuplicateDetector::DuplicateDetector(const Options &options)
    : m_options(options) {}

bool DuplicateDetector::record(std::string_view client_id,
                               std::string_view body_hash,
                               std::string_view body) {
  if (!m_options.m_enabled) {
    return false;
  }
  const uint64_t now_ms = TimeUtils::steady_ms();
  std::lock_guard<std::mutex> lock(m_mutex);
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
    return false;
  }

  Entry &entry = it->second;
  entry.m_count += 1;
  entry.m_last_seen_ms = now_ms;
  entry.m_client_ids.insert(std::string(client_id));
  if (entry.m_body.empty() && body.size() <= m_options.m_max_body_bytes) {
    entry.m_body = std::string(body);
  }
  return entry.m_count >= 2;
}

size_t DuplicateDetector::duplicate_bodies() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<size_t>(
      std::count_if(m_entries.begin(), m_entries.end(),
                    [](const auto &kv) { return kv.second.m_count >= 2; }));
}

nlohmann::json DuplicateDetector::report() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  nlohmann::json result;
  result["enabled"] = m_options.m_enabled; //-V601 nlohmann::json handles bool

  std::vector<const Entry *> duplicates;
  size_t duplicate_occurrences = 0;
  size_t same_client = 0;
  size_t cross_client = 0;
  for (const auto &[hash, entry] : m_entries) {
    if (entry.m_count < 2) {
      continue;
    }
    duplicates.push_back(&entry);
    duplicate_occurrences += entry.m_count - 1;
    if (entry.m_client_ids.size() <= 1) {
      ++same_client;
    } else {
      ++cross_client;
    }
  }

  std::sort(duplicates.begin(), duplicates.end(),
            [](const Entry *a, const Entry *b) {
              if (a->m_count != b->m_count) {
                return a->m_count > b->m_count;
              }
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
}

void DuplicateDetector::evict_lowest_count_locked() {
  if (m_entries.empty()) {
    return;
  }
  auto victim = m_entries.begin();
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
    if (it->second.m_count < victim->second.m_count ||
        (it->second.m_count == victim->second.m_count &&
         it->second.m_first_seen_ms < victim->second.m_first_seen_ms)) {
      victim = it;
    }
  }
  m_entries.erase(victim);
}

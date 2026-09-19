#include "http_client_pool.hpp"
#include "logger.hpp"
#include <chrono>
#include <stdexcept>
#include <thread>

HttpClientPool::HttpClientPool(size_t max_pool_size, int timeout_seconds,
                               int acquire_timeout_seconds,
                               bool enable_connection_reuse,
                               bool enable_ssl_server_certificate_verification,
                               bool enable_ssl_server_hostname_verification,
                               const std::string &ssl_ca_cert_path,
                               int max_idle_timeout_seconds)
    : m_config{max_pool_size, timeout_seconds, acquire_timeout_seconds,
               enable_connection_reuse,
               enable_ssl_server_certificate_verification,
               enable_ssl_server_hostname_verification, ssl_ca_cert_path,
               std::chrono::seconds(max_idle_timeout_seconds)},
      m_state{},
      m_counters{} {
  Logger::debug("HttpClientPool created: max_size={} timeout={}s "
                "acquire_timeout={}s reuse={} idle_timeout={}s",
                m_config.m_max_pool_size, m_config.m_timeout_seconds, m_config.m_acquire_timeout_seconds,
                m_config.m_enable_connection_reuse, max_idle_timeout_seconds);
}

void HttpClientPool::set_metrics(const PoolMetrics &metrics) {
  m_metrics = metrics;
}

void HttpClientPool::update_metrics() {
  if (m_metrics.m_active_clients) {
    m_metrics.m_active_clients->get().Set(
        static_cast<double>(m_counters.m_active_clients.load()));
  }
  if (m_metrics.m_available_clients) {
    m_metrics.m_available_clients->get().Set(
        static_cast<double>(m_state.m_available_connections.size()));
  }
}

std::unique_ptr<HttpClient> HttpClientPool::acquire_connection() {
  const auto start_time = std::chrono::steady_clock::now();
  std::unique_lock lock(m_state.m_pool_mutex);

  // Try to get a connection from the pool first
  auto acquired = try_acquire_from_queue(start_time);
  if (acquired) {
    return acquired;
  }

  // Check if we can create a new connection
  if (m_counters.m_total_clients >= m_config.m_max_pool_size) {
    // Wait for a connection with timeout (no recursion!)
    Logger::debug("HttpClientPool: pool full, waiting for available connection "
                  "(timeout={}s)",
                  m_config.m_acquire_timeout_seconds);

    const bool got_connection = m_state.m_condition.wait_for(
        lock, std::chrono::seconds(m_config.m_acquire_timeout_seconds),
        [this] { return !m_state.m_available_connections.empty(); });

    if (!got_connection) {
      // Timeout - log and throw
      Logger::error(
          "HttpClientPool: acquire timeout after {}s (max_size={}, total={})",
          m_config.m_acquire_timeout_seconds, m_config.m_max_pool_size, m_counters.m_total_clients.load());

      if (m_metrics.m_acquisition_timeouts) {
        m_metrics.m_acquisition_timeouts->get().Increment();
      }

      throw std::runtime_error(
          "HTTP pool acquire timeout - all connections in use");
    }

    // Successfully waited - try to acquire again
    acquired = try_acquire_from_queue(start_time);
    if (acquired) {
      return acquired;
    }
  }

  // Create new connection (pool not full or connections were invalid)
  Logger::debug("HttpClientPool: creating new connection (total={}/{})",
                m_counters.m_total_clients.load() + 1, m_config.m_max_pool_size);

  auto client = std::make_unique<HttpClient>(
      m_config.m_timeout_seconds,
      m_config.m_enable_connection_reuse, // Now true by default!
      m_config.m_enable_ssl_server_certificate_verification,
      m_config.m_enable_ssl_server_hostname_verification, m_config.m_ssl_ca_cert_path);

  m_counters.m_total_clients++;
  m_counters.m_active_clients++;

  update_metrics();

  record_acquisition(start_time);

  return client;
}

std::unique_ptr<HttpClient> HttpClientPool::try_acquire_from_queue(
    std::chrono::steady_clock::time_point start_time) {
  // Caller must hold m_state.m_pool_mutex
  while (!m_state.m_available_connections.empty()) {
    auto client = std::move(m_state.m_available_connections.front());
    m_state.m_available_connections.pop();

    // Check if connection is stale (idle too long)
    if (client && std::chrono::steady_clock::now() - client->get_last_used() >
                      m_config.m_max_idle_time) {
      m_counters.m_total_clients--;
      m_counters.m_stale_evictions++;
      if (m_metrics.m_stale_evictions) {
        m_metrics.m_stale_evictions->get().Increment();
      }
      Logger::warn("HttpClientPool: evicting stale connection (idle > {}s), "
                   "stale_evictions={}",
                   m_config.m_max_idle_time.count(), m_counters.m_stale_evictions.load());
      continue;
    }

    m_counters.m_active_clients++;

    update_metrics();

    // Validate the connection
    if (client && client->is_valid()) {
      record_acquisition(start_time);

      Logger::debug("HttpClientPool: acquired connection from pool (active={})",
                    m_counters.m_active_clients.load());
      return client;
    }

    // Connection invalid - destroy it and continue
    m_counters.m_total_clients--;
    m_counters.m_active_clients--;
    Logger::warn("HttpClientPool: invalid connection in pool, discarding");
  }

  return nullptr;
}

void HttpClientPool::release_connection(std::unique_ptr<HttpClient> client) {
  if (!client)
    return;

  // Decrement active count first
  m_counters.m_active_clients--;

  if (!client->is_valid()) {
    std::lock_guard lock(m_state.m_pool_mutex);
    m_counters.m_total_clients--;
    Logger::warn(
        "HttpClientPool: released invalid connection, destroying (active={})",
        m_counters.m_active_clients.load());
    update_metrics();
    return;
  }

  std::lock_guard lock(m_state.m_pool_mutex);

  if (m_state.m_available_connections.size() < m_config.m_max_pool_size) {
    client->touch();
    m_state.m_available_connections.push(std::move(client));
    m_state.m_condition.notify_one();
    Logger::debug(
        "HttpClientPool: connection released to pool (active={}, available={})",
        m_counters.m_active_clients.load(), m_state.m_available_connections.size());
  } else {
    // Pool is full - destroy the connection
    m_counters.m_total_clients--;
    Logger::debug(
        "HttpClientPool: pool full, released connection destroyed (active={})",
        m_counters.m_active_clients.load());
  }

  update_metrics();

  if (m_metrics.m_releases) {
    m_metrics.m_releases->get().Increment();
  }
}

size_t HttpClientPool::available_count() const {
  std::lock_guard lock(m_state.m_pool_mutex);
  return m_state.m_available_connections.size();
}

void HttpClientPool::record_acquisition(
    std::chrono::steady_clock::time_point start_time) {
  if (m_metrics.m_acquisitions) {
    m_metrics.m_acquisitions->get().Increment();
  }

  if (m_metrics.m_acquisition_duration) {
    const auto duration = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - start_time)
                              .count();
    m_metrics.m_acquisition_duration->get().Observe(duration);
  }
}

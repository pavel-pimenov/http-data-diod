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
    : m_max_pool_size(max_pool_size), m_timeout_seconds(timeout_seconds),
      m_acquire_timeout_seconds(acquire_timeout_seconds),
      m_enable_connection_reuse(enable_connection_reuse),
      m_enable_ssl_server_certificate_verification(
          enable_ssl_server_certificate_verification),
      m_enable_ssl_server_hostname_verification(
          enable_ssl_server_hostname_verification),
      m_ssl_ca_cert_path(ssl_ca_cert_path),
      m_max_idle_time(std::chrono::seconds(max_idle_timeout_seconds)) {
  Logger::debug("HttpClientPool created: max_size={} timeout={}s "
                "acquire_timeout={}s reuse={} idle_timeout={}s",
                m_max_pool_size, m_timeout_seconds, m_acquire_timeout_seconds,
                m_enable_connection_reuse, max_idle_timeout_seconds);
}

void HttpClientPool::set_metrics(prometheus::Gauge *active_clients,
                                 prometheus::Gauge *available_clients,
                                 prometheus::Counter *acquisitions,
                                 prometheus::Counter *releases,
                                 prometheus::Counter *acquisition_timeouts,
                                 prometheus::Histogram *acquisition_duration,
                                 prometheus::Counter *stale_evictions) {
  m_active_clients_gauge = active_clients;
  m_available_clients_gauge = available_clients;
  m_acquisitions_counter = acquisitions;
  m_releases_counter = releases;
  m_acquisition_timeouts_counter = acquisition_timeouts;
  m_acquisition_duration_histogram = acquisition_duration;
  m_stale_evictions_counter = stale_evictions;
}

void HttpClientPool::update_metrics() {
  if (m_active_clients_gauge) {
    m_active_clients_gauge->Set(static_cast<double>(m_active_clients.load()));
  }
  if (m_available_clients_gauge) {
    m_available_clients_gauge->Set(
        static_cast<double>(m_available_connections.size()));
  }
}

std::unique_ptr<HttpClient> HttpClientPool::acquire_connection() {
  const auto start_time = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lock(m_pool_mutex);

  // Try to get a connection from the pool first
  auto acquired = try_acquire_from_queue(start_time);
  if (acquired) {
    return acquired;
  }

  // Check if we can create a new connection
  if (m_total_clients >= m_max_pool_size) {
    // Wait for a connection with timeout (no recursion!)
    Logger::debug("HttpClientPool: pool full, waiting for available connection "
                  "(timeout={}s)",
                  m_acquire_timeout_seconds);

    const bool got_connection = m_condition.wait_for(
        lock, std::chrono::seconds(m_acquire_timeout_seconds),
        [this] { return !m_available_connections.empty(); });

    if (!got_connection) {
      // Timeout - log and throw
      Logger::error(
          "HttpClientPool: acquire timeout after {}s (max_size={}, total={})",
          m_acquire_timeout_seconds, m_max_pool_size, m_total_clients.load());

      if (m_acquisition_timeouts_counter) {
        m_acquisition_timeouts_counter->Increment();
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
                m_total_clients.load() + 1, m_max_pool_size);

  auto client = std::make_unique<HttpClient>(
      m_timeout_seconds,
      m_enable_connection_reuse, // Now true by default!
      m_enable_ssl_server_certificate_verification,
      m_enable_ssl_server_hostname_verification, m_ssl_ca_cert_path);

  m_total_clients++;
  m_active_clients++;

  update_metrics();

  if (m_acquisitions_counter) {
    m_acquisitions_counter->Increment();
  }

  if (m_acquisition_duration_histogram) {
    const auto duration = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - start_time)
                              .count();
    m_acquisition_duration_histogram->Observe(duration);
  }

  return client;
}

std::unique_ptr<HttpClient> HttpClientPool::try_acquire_from_queue(
    std::chrono::steady_clock::time_point start_time) {
  // Caller must hold m_pool_mutex
  while (!m_available_connections.empty()) {
    auto client = std::move(m_available_connections.front());
    m_available_connections.pop();

    // Check if connection is stale (idle too long)
    if (client && std::chrono::steady_clock::now() - client->get_last_used() >
                      m_max_idle_time) {
      m_total_clients--;
      m_stale_evictions++;
      if (m_stale_evictions_counter) {
        m_stale_evictions_counter->Increment();
      }
      Logger::warn("HttpClientPool: evicting stale connection (idle > {}s), "
                   "stale_evictions={}",
                   m_max_idle_time.count(), m_stale_evictions.load());
      continue;
    }

    m_active_clients++;

    update_metrics();

    // Validate the connection
    if (client && client->is_valid()) {
      if (m_acquisitions_counter) {
        m_acquisitions_counter->Increment();
      }

      if (m_acquisition_duration_histogram) {
        const auto duration = std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - start_time)
                                  .count();
        m_acquisition_duration_histogram->Observe(duration);
      }

      Logger::debug("HttpClientPool: acquired connection from pool (active={})",
                    m_active_clients.load());
      return client;
    }

    // Connection invalid - destroy it and continue
    m_total_clients--;
    m_active_clients--;
    Logger::warn("HttpClientPool: invalid connection in pool, discarding");
  }

  return nullptr;
}

void HttpClientPool::release_connection(std::unique_ptr<HttpClient> client) {
  if (!client)
    return;

  // Decrement active count first
  m_active_clients--;

  if (!client->is_valid()) {
    std::lock_guard<std::mutex> lock(m_pool_mutex);
    m_total_clients--;
    Logger::warn(
        "HttpClientPool: released invalid connection, destroying (active={})",
        m_active_clients.load());
    update_metrics();
    return;
  }

  std::lock_guard<std::mutex> lock(m_pool_mutex);

  if (m_available_connections.size() < m_max_pool_size) {
    client->touch();
    m_available_connections.push(std::move(client));
    m_condition.notify_one();
    Logger::debug(
        "HttpClientPool: connection released to pool (active={}, available={})",
        m_active_clients.load(), m_available_connections.size());
  } else {
    // Pool is full - destroy the connection
    m_total_clients--;
    Logger::debug(
        "HttpClientPool: pool full, released connection destroyed (active={})",
        m_active_clients.load());
  }

  update_metrics();

  if (m_releases_counter) {
    m_releases_counter->Increment();
  }
}

size_t HttpClientPool::available_count() const {
  std::lock_guard<std::mutex> lock(m_pool_mutex);
  return m_available_connections.size();
}

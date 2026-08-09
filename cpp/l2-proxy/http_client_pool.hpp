#ifndef HTTP_CLIENT_POOL_HPP
#define HTTP_CLIENT_POOL_HPP

#include "http_client.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>

// Optimized HTTP Client Pool with per-host connection reuse
// Key improvements:
// - Timeout on acquire to prevent indefinite blocking
// - Connection reuse enabled by default (keep-alive)
// - Better metrics for monitoring
class HttpClientPool {
public:
  using client_type = HttpClient;

private:
  std::queue<std::unique_ptr<HttpClient>> m_available_connections;
  mutable std::mutex m_pool_mutex;
  std::condition_variable m_condition;
  size_t m_max_pool_size;
  int m_timeout_seconds;
  int m_acquire_timeout_seconds; // Timeout for waiting on available connection
  bool m_enable_connection_reuse;
  bool m_enable_ssl_server_certificate_verification;
  bool m_enable_ssl_server_hostname_verification;
  std::string m_ssl_ca_cert_path;
  std::atomic<size_t> m_total_clients{0};
  std::atomic<size_t> m_active_clients{
      0}; // Currently in use (acquired but not released)
  std::chrono::seconds m_max_idle_time{300};
  std::atomic<size_t> m_stale_evictions{0};

  prometheus::Gauge *m_active_clients_gauge = nullptr;
  prometheus::Gauge *m_available_clients_gauge = nullptr;
  prometheus::Counter *m_acquisitions_counter = nullptr;
  prometheus::Counter *m_releases_counter = nullptr;
  prometheus::Counter *m_acquisition_timeouts_counter = nullptr;
  prometheus::Histogram *m_acquisition_duration_histogram = nullptr;
  prometheus::Counter *m_stale_evictions_counter = nullptr;

public:
  // Constructor with acquire timeout (default 30 seconds)
  explicit HttpClientPool(
      size_t max_pool_size = 10, int timeout_seconds = 10,
      int acquire_timeout_seconds = 30,
      bool enable_connection_reuse = true, // Changed default to true
      bool enable_ssl_server_certificate_verification = false,
      bool enable_ssl_server_hostname_verification = false,
      const std::string &ssl_ca_cert_path = "",
      int max_idle_timeout_seconds = 300);

  void set_metrics(prometheus::Gauge *active_clients,
                   prometheus::Gauge *available_clients,
                   prometheus::Counter *acquisitions,
                   prometheus::Counter *releases,
                   prometheus::Counter *acquisition_timeouts = nullptr,
                   prometheus::Histogram *acquisition_duration = nullptr,
                   prometheus::Counter *stale_evictions = nullptr);

  // Acquire connection with timeout
  [[nodiscard]] std::unique_ptr<HttpClient> acquire_connection();

  void release_connection(std::unique_ptr<HttpClient> client);

  size_t available_count() const;
  size_t total_clients() const { return m_total_clients.load(); }
  size_t active_clients() const { return m_active_clients.load(); }

private:
  void update_metrics();
  std::unique_ptr<HttpClient>
  try_acquire_from_queue(std::chrono::steady_clock::time_point start_time);
};

#endif // HTTP_CLIENT_POOL_HPP

#include "stats_logger.hpp"
#include "logger.hpp"

StatsLogger::StatsLogger(AppContext &context, std::atomic<bool> &shutdown_flag)
    : m_app_ctx(context), m_shutdown_flag(shutdown_flag),
      m_start_time(std::chrono::steady_clock::now()) {}

StatsLogger::~StatsLogger() {
  if (m_log_thread.joinable()) {
    m_log_thread.request_stop();
    m_cv.notify_all();
    m_log_thread.join();
  }
}

void StatsLogger::increment_active_clients() {
  uint64_t current = m_active_clients.fetch_add(1) + 1;
  uint64_t max = m_max_clients.load(std::memory_order_relaxed);
  // Single CAS attempt — slightly stale max is acceptable for stats
  m_max_clients.compare_exchange_weak(max, current, std::memory_order_relaxed);
}

void StatsLogger::decrement_active_clients() { m_active_clients.fetch_sub(1); }

void StatsLogger::increment_total_requests() { m_total_requests.fetch_add(1); }

void StatsLogger::start_periodic_logging() {
  Logger::info("Starting statistics logging every 600 seconds");
  m_log_thread = std::jthread([this](std::stop_token st) {
    uint64_t prev_logged_requests = 0;
    auto prev_time = std::chrono::steady_clock::now();

    while (!m_shutdown_flag && !st.stop_requested()) {
      {
        std::unique_lock lk(m_cv_mutex);
        // C++20 jthread + condition_variable_any: wait_for with stop_token wakes
        // instantly on request_stop(), no 1s polling spin.
        m_cv.wait_for(lk, st, std::chrono::seconds(600),
                      [&] { return st.stop_requested() || m_shutdown_flag.load(); });
      }
      if (m_shutdown_flag || st.stop_requested()) {
        break;
      }

      if (!m_shutdown_flag) {
        // Get current values from Prometheus metrics based on mode
        uint64_t bytes_received = 0;
        uint64_t bytes_sent = 0;
        uint64_t client_requests = 0;
        uint64_t client_errors = 0;
        uint64_t nats_requests = 0;
        uint64_t nats_errors = 0;

        if (m_app_ctx.m_config.m_mode == "proxy") {
          bytes_received = static_cast<uint64_t>(
              m_app_ctx.m_proxy.m_metrics->m_bytes_received.Value());
          bytes_sent = static_cast<uint64_t>(
              m_app_ctx.m_proxy.m_metrics->m_bytes_sent.Value());
          client_requests = static_cast<uint64_t>(
              m_app_ctx.m_proxy.m_metrics->m_client_requests.Value());
          client_errors = static_cast<uint64_t>(
              m_app_ctx.m_proxy.m_metrics->m_client_errors.Value());

          nats_requests = static_cast<uint64_t>(
              m_app_ctx.m_proxy.m_metrics->m_nats_requests.Value());
          nats_errors = static_cast<uint64_t>(
              m_app_ctx.m_proxy.m_metrics->m_nats_errors.Value());
        } else if (m_app_ctx.m_config.m_mode == "worker") {
          bytes_received = static_cast<uint64_t>(
              m_app_ctx.m_worker.m_metrics->m_bytes_received.Value());
          bytes_sent = static_cast<uint64_t>(
              m_app_ctx.m_worker.m_metrics->m_bytes_sent.Value());
          client_requests = static_cast<uint64_t>(
              m_app_ctx.m_worker.m_metrics->m_requests_processed.Value());
          client_errors = static_cast<uint64_t>(
              m_app_ctx.m_worker.m_metrics->m_l2_errors.Value());

        } else if (m_app_ctx.m_config.m_mode == "l2-server") {
          bytes_received = static_cast<uint64_t>(
              m_app_ctx.m_server.m_metrics->m_bytes_received.Value());
          bytes_sent = static_cast<uint64_t>(
              m_app_ctx.m_server.m_metrics->m_bytes_sent.Value());
          client_requests = static_cast<uint64_t>(
              m_app_ctx.m_server.m_metrics->m_requests.Value());
          client_errors = static_cast<uint64_t>(
              m_app_ctx.m_server.m_metrics->m_request_errors.Value());
        }

        uint64_t active = m_active_clients.load();
        uint64_t max = m_max_clients.load();
        uint64_t current_requests_for_rate = client_requests;

        if (m_app_ctx.m_config.m_mode == "proxy") {
          current_requests_for_rate = nats_requests;
        }

        // Calculate requests per second over the last logging period
        const auto now = std::chrono::steady_clock::now();
        const auto duration =
            std::chrono::duration_cast<std::chrono::seconds>(now - prev_time)
                .count();
        uint64_t requests_in_period =
            current_requests_for_rate >= prev_logged_requests
                ? (current_requests_for_rate - prev_logged_requests)
                : 0;
        double requests_per_second =
            duration > 0 ? static_cast<double>(requests_in_period) / duration
                         : 0.0;

        prev_logged_requests = current_requests_for_rate;
        prev_time = now;

        const bool show_common_stats =
            bytes_received > 0 || bytes_sent > 0 || client_requests > 0 ||
            client_errors > 0 || requests_in_period > 0;

        if (show_common_stats) {
          Logger::info("Statistics - Bytes Received: {} bytes, Bytes Sent: {} "
                       "bytes, Client Requests: {}, Client Errors: {}",
                       bytes_received, bytes_sent, client_requests,
                       client_errors);
        }

        if (m_app_ctx.m_config.m_mode == "proxy") {
          Logger::info("Statistics - NATS Requests: {}, NATS Errors: {}",
                       nats_requests, nats_errors);
        }

        Logger::info("Statistics - Active Clients: {}, Max Clients: {}, "
                     "Requests in last 600s: {}, Req/Sec (last 600s): {:.2f}",
                     active, max, requests_in_period, requests_per_second);
      }
    }
  });
}
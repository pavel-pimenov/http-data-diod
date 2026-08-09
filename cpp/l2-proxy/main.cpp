#include "crash_handler.hpp"
#include "logger.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#include "cpp_httplib_config.h"
#include "httplib/httplib.h"

#include <prometheus/exposer.h>

#include "nlohmann/json.hpp"

#include "app_context.hpp"
#include "l2_worker.hpp"
#include "labeled_counter_collector.hpp"
#include "request_handler.hpp"
#include "server_handler.hpp"
#include "stats_logger.hpp"

extern const char *g_l2_proxy_version;

using json = nlohmann::json;

std::atomic<bool> g_shutdown_flag{false};
std::atomic<int> g_signal_number{0};
std::mutex g_shutdown_mutex;
std::condition_variable g_shutdown_cv;

// Helper function to configure httplib server with common settings
void configure_httplib_server(httplib::Server &server, const Config &config) {
  // Configure server for better performance - match previous CivetServer
  // settings
  server.set_keep_alive_max_count(100); // Allow more keep-alive connections
  server.set_keep_alive_timeout(5);     // Keep connections alive for 5 seconds
  server.set_read_timeout(config.m_request_timeout_seconds,
                          0); // Match m_request_timeout_seconds from config
  server.set_write_timeout(config.m_http_timeout_seconds,
                           0);  // Match http_timeout_seconds from config
  server.set_tcp_nodelay(true); // Disable Nagle's algorithm for lower latency
  server.set_payload_max_length(
      static_cast<size_t>(10) * 1024 *
      1024); // 10MB max payload to handle larger requests
}

template <typename ServerType, typename HandlerType>
void run_server(ServerType &server, AppContext &app_ctx, HandlerType &handler,
                int port, const std::string &server_name,
                bool use_in_flight_tracker = false) {

  // Register request handlers
  server.Get(R"(/.*)",
             [&](const httplib::Request &req, httplib::Response &res) {
               handler.handle_get(req, res);
             });

  server.Post(R"(/.*)",
              [&](const httplib::Request &req, httplib::Response &res) {
                handler.handle_post(req, res);
              });

  // Start server in a separate thread to allow signal handling
  std::thread server_thread([&]() {
    Logger::info("{} server thread started, listening on port {}", server_name,
                 port);
    try {
      bool listen_result = server.listen("0.0.0.0", port);
      if (!listen_result) {
        Logger::error(
            "{} server.listen() returned false - port binding failed!",
            server_name);
      } else {
        Logger::info(
            "{} server.listen() returned true - server started successfully",
            server_name);
      }
    } catch (const std::exception &e) {
      handle_error(std::format("{} server.listen() threw exception: {}",
                               server_name, e.what()));
    } catch (...) {
      handle_error(std::format("{} server.listen() threw unknown exception",
                               server_name));
    }
    Logger::info("{} server thread finished", server_name);
  });

  // Wait for shutdown signal — check flag every 100ms via condition_variable
  Logger::info("Main thread waiting for shutdown signal...");
  {
    std::unique_lock<std::mutex> lock(g_shutdown_mutex);
    while (!g_shutdown_flag.load()) {
      g_shutdown_cv.wait_for(lock, std::chrono::milliseconds(100));
    }
    Logger::info("Received signal {}, exiting...", g_signal_number.load());
  }

  // Graceful shutdown
  Logger::info("Shutdown signal received, stopping {} server...", server_name);
  server.stop();

  // Wait for in-flight requests if needed (proxy mode)
  if (use_in_flight_tracker) {
    Logger::info("Waiting for in-flight requests to complete...");
    if (app_ctx.m_in_flight_tracker.wait_for_completion(
            std::chrono::seconds(30))) {
      Logger::info("All in-flight requests completed gracefully");
    } else {
      Logger::warn("Some in-flight requests did not complete within timeout");
    }
  }

  Logger::info("{} server stop() called, waiting for server thread to join...",
               server_name);
  if (server_thread.joinable()) {
    server_thread.join();
    Logger::info("{} server thread joined successfully", server_name);
  }
}

std::unique_ptr<prometheus::Exposer>
create_metrics_exposer(int port,
                       const std::shared_ptr<prometheus::Registry> &registry) {
  auto exposer =
      std::make_unique<prometheus::Exposer>(std::format("0.0.0.0:{}", port));
  exposer->RegisterCollectable(registry);
  return exposer;
}

void run_proxy(AppContext &app_ctx) {
  StatsLogger stats_logger(app_ctx, g_shutdown_flag);
  stats_logger
      .start_periodic_logging(); // Start periodic logging in proxy mode too
  RequestHandler request_handler(app_ctx, &stats_logger);

  auto exposer = create_metrics_exposer(19090, app_ctx.m_proxy_registry);

  if (app_ctx.m_proxy.m_per_ip_metrics_collector) {
    exposer->RegisterCollectable(app_ctx.m_proxy.m_per_ip_metrics_collector);
  }

  if (app_ctx.m_proxy.m_per_client_id_metrics_collector) {
    exposer->RegisterCollectable(
        app_ctx.m_proxy.m_per_client_id_metrics_collector);
  }

  if (app_ctx.m_proxy.m_per_client_id_duplicate_collector) {
    exposer->RegisterCollectable(
        app_ctx.m_proxy.m_per_client_id_duplicate_collector);
  }

  if (app_ctx.m_proxy.m_per_client_id_latency_collector) {
    exposer->RegisterCollectable(
        app_ctx.m_proxy.m_per_client_id_latency_collector);
  }

  Logger::info("C++ DMZ Proxy listening on {}://0.0.0.0:{}",
               app_ctx.m_config.m_proxy_protocol,
               app_ctx.m_config.m_proxy_port);
  Logger::info("Prometheus metrics available at http://0.0.0.0:19090/metrics");

  if (app_ctx.m_config.m_proxy_protocol == "https") {
    // HTTPS mode
    httplib::SSLServer server(app_ctx.m_config.m_ssl_server_cert_file.c_str(),
                              app_ctx.m_config.m_ssl_server_key_file.c_str());
    configure_httplib_server(server, app_ctx.m_config);
    run_server(server, app_ctx, request_handler, app_ctx.m_config.m_proxy_port,
               "httplib proxy", true);
  } else {
    // HTTP mode
    httplib::Server server;
    configure_httplib_server(server, app_ctx.m_config);
    run_server(server, app_ctx, request_handler, app_ctx.m_config.m_proxy_port,
               "httplib", true);
  }
}

void run_worker(AppContext &app_ctx) {
  StatsLogger stats_logger(app_ctx, g_shutdown_flag);
  stats_logger.start_periodic_logging();

  auto exposer = create_metrics_exposer(19091, app_ctx.m_worker_registry);

  L2Worker worker(app_ctx);

  // Start health check HTTP server on separate port
  httplib::Server health_server;
  health_server.Get("/health/live", [](const httplib::Request & /*req*/,
                                       httplib::Response &res) {
    set_health_alive(res, "l2-worker");
  });
  health_server.Get("/health/ready", [&worker](const httplib::Request & /*req*/,
                                               httplib::Response &res) {
    const bool ready = worker.is_nats_connected();
    if (ready) {
      res.status = 200;
      res.set_content(
          R"({"status": "ready", "service": "l2-worker", "messaging": "nats"})",
          "application/json");
    } else {
      res.status = 503;
      res.set_content(
          R"({"status": "not_ready", "service": "l2-worker", "error": "NATS not connected"})",
          "application/json");
    }
  });

  std::thread health_thread([&health_server]() {
    Logger::info("Worker health server started on port 19093");
    health_server.listen("0.0.0.0", 19093);
  });

  Logger::info("C++ L2 Worker Prometheus metrics available at "
               "http://0.0.0.0:19091/metrics");
  Logger::info("C++ L2 Worker health check available at "
               "http://0.0.0.0:19093/health/ready");
  worker.run();

  health_server.stop();
  if (health_thread.joinable()) {
    health_thread.join();
  }
}

void run_l2_server(AppContext &app_ctx) {
  StatsLogger stats_logger(app_ctx, g_shutdown_flag);
  stats_logger.start_periodic_logging();

  ServerHandler server_handler(app_ctx);

  auto exposer = create_metrics_exposer(19092, app_ctx.m_server_registry);

  Logger::info("C++ L2 Server listening on {}://0.0.0.0:{}",
               app_ctx.m_config.m_l2_server_protocol,
               app_ctx.m_config.m_l2_server_port);
  Logger::info("C++ L2 Server Prometheus metrics available at "
               "http://0.0.0.0:19092/metrics");

  Logger::info("Using cpp-httplib for L2 server");

  if (app_ctx.m_config.m_l2_server_protocol == "https") {
    // HTTPS mode
    httplib::SSLServer server(app_ctx.m_config.m_ssl_server_cert_file.c_str(),
                              app_ctx.m_config.m_ssl_server_key_file.c_str());
    configure_httplib_server(server, app_ctx.m_config);
    run_server(server, app_ctx, server_handler,
               app_ctx.m_config.m_l2_server_port, "cpp-httplib SSL", false);
  } else {
    // HTTP mode
    httplib::Server server;
    configure_httplib_server(server, app_ctx.m_config);
    run_server(server, app_ctx, server_handler,
               app_ctx.m_config.m_l2_server_port, "cpp-httplib", false);
  }
}

void init_tracer(AppContext &app_ctx) {
  Logger::info("init_tracer called: enable_tracing={} jaeger_url='{}'",
               app_ctx.m_config.m_enable_tracing,
               app_ctx.m_config.m_jaeger_url);

  if (app_ctx.m_config.m_enable_tracing &&
      !app_ctx.m_config.m_jaeger_url.empty()) {
    Logger::info("Creating JaegerLogger with config: batch_size={} "
                 "flush_interval={}ms sample_rate={}",
                 app_ctx.m_config.m_tracing_batch_size,
                 app_ctx.m_config.m_tracing_flush_interval_ms,
                 app_ctx.m_config.m_tracing_sample_rate);
    app_ctx.m_tracer = std::make_unique<JaegerLogger>(
        app_ctx.m_config.m_jaeger_url, app_ctx.m_tracing_metrics->m_spans_sent,
        app_ctx.m_tracing_metrics->m_spans_failed,
        app_ctx.m_tracing_metrics->m_queue_size,
        app_ctx.m_tracing_metrics->m_last_send_duration,
        app_ctx.m_tracing_metrics->m_send_latency,
        app_ctx.m_tracing_metrics->m_queue_time,
        app_ctx.m_config.m_tracing_batch_size,
        app_ctx.m_config.m_tracing_flush_interval_ms,
        app_ctx.m_config.m_tracing_sample_rate);
    Logger::info("JAEGER_URL set, tracing enabled: {}",
                 app_ctx.m_config.m_jaeger_url);
  } else {
    Logger::info("Tracing disabled");
  }

  Logger::info("init_tracer finished: tracer is {}",
               app_ctx.m_tracer ? "initialized" : "null");
}

void signal_handler(int signum) {
  g_signal_number.store(signum);
  g_shutdown_flag.store(true);
}

// NOLINTNEXTLINE(bugprone-exception-escape) - startup calls before try block
// may throw; they terminate the process intentionally on startup failure.
int main() { // NOLINT(bugprone-exception-escape)
  std::signal(SIGTERM, signal_handler);
  std::signal(SIGINT, signal_handler);

  // Install crash handler for stack trace dumps
  const char *crash_dump_dir = std::getenv("CRASH_DUMP_DIR");
  CrashHandler::install(crash_dump_dir ? crash_dump_dir
                                       : g_default_crash_dump_dir);

  try {
    Logger::info("L2-Proxy version: {}", g_l2_proxy_version);

#ifndef L2_PROXY_BUILD_MODE
#define L2_PROXY_BUILD_MODE "unknown"
#endif
    Logger::info("Build mode: {}", L2_PROXY_BUILD_MODE);
    const char *asan_opts = std::getenv("ASAN_OPTIONS");
    if (asan_opts) {
      Logger::info("ASAN_OPTIONS: {}", asan_opts);
    }
    const char *lsan_opts = std::getenv("LSAN_OPTIONS");
    if (lsan_opts) {
      Logger::info("LSAN_OPTIONS: {}", lsan_opts);
    }

    AppContext app_ctx;
    init_tracer(app_ctx);

    // Crash test mode: raise SIGSEGV to test crash handler
    if (app_ctx.m_config.m_crash_test) {
      Logger::info(
          "CRASH_TEST mode enabled - raising SIGSEGV to test crash handler");
      spdlog::default_logger()->flush();
      // Write test file to confirm crash handler directory is accessible
      {
        const char *dir =
            crash_dump_dir ? crash_dump_dir : g_default_crash_dump_dir;
        std::string test_file = std::string(dir) + "/crash_test_marker.txt";
        std::ofstream f(test_file);
        if (f.is_open()) {
          f << "crash handler test marker\n";
          f.close();
          Logger::info("Crash dump directory writable: {}", dir);
        } else {
          Logger::error("Cannot write to crash dump directory: {}", dir);
        }
      }
      spdlog::default_logger()->flush();
      // Trigger crash
      volatile int *bad_ptr = nullptr;
      *bad_ptr = 42; // NOLINT — intentional crash for testing
    }

    if (app_ctx.m_config.m_mode == "proxy") {
      Logger::info("Starting in proxy mode");
      run_proxy(app_ctx);
    } else if (app_ctx.m_config.m_mode == "worker") {
      Logger::info("Starting in worker mode");
      run_worker(app_ctx);
    } else if (app_ctx.m_config.m_mode == "l2-server") {
      Logger::info("Starting in l2-server mode");
      run_l2_server(app_ctx);
    } else {
      handle_error(
          std::format("Invalid mode: {}. Use proxy, worker, or l2-server",
                      app_ctx.m_config.m_mode));
      return 1;
    }
  } catch (const std::exception &e) {
    handle_error(e.what());
    return 1;
  } catch (...) {
    handle_error("Unknown error");
    return 1;
  }

  return 0;
}

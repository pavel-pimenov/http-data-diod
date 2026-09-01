#include "common_utils.hpp"
#include "db_query_utils.hpp"
#include "json_utils.hpp"
#include "l2_worker.hpp"
#include "logger.hpp"
#include "metrics_manager.hpp"
#include "retry_utils.hpp"
#include "time_utils.hpp"
#include "trace_logger.hpp"
#include "tracing_helpers.hpp"
#include <base64.hpp>
#include <chrono>
#include <nlohmann/json.hpp>
#include <thread>

namespace {
// Logs the worker-side NATS_consume span. Shared by the main and DB request
// handlers, which duplicated the messaging.* attributes + log_span_to_jaeger
// pair.
void log_nats_consume_span(JaegerLogger *tracer, const std::string &request_id,
                           const std::string &trace_id, uint64_t start_us,
                           const std::string &consume_span_id,
                           const std::string &proxy_span_id,
                           const std::string &service_name,
                           const std::string &destination,
                           const std::string &reply_to) {
  if (!tracer || trace_id.empty()) {
    return;
  }
  nlohmann::json attrs = {
      {"messaging.system", "nats"},
      {"messaging.operation", "consume"},
      {"messaging.destination", destination},
      {"messaging.reply_to", reply_to},
  };
  log_span_to_jaeger(tracer, "NATS_consume", "/nats", 200, start_us, start_us,
                     service_name, request_id, trace_id, consume_span_id,
                     proxy_span_id, attrs);
}

// Builds the response headers carrying the worker's consume span id (or an
// empty map when absent). Shared by the main and DB response paths.
NatsHeaders make_consume_span_headers(const std::string &consume_span_id) {
  NatsHeaders response_headers;
  if (!consume_span_id.empty()) {
    response_headers[NatsContract::kConsumeSpanIdHeader] = consume_span_id;
  }
  return response_headers;
}
} // namespace

extern std::atomic<bool> g_shutdown_flag;

template <typename Fn>
bool L2Worker::subscribe_nats_subject(const std::string &subject,
                                      const std::string &queue_group,
                                      const std::string &error_context,
                                      Fn &&fn) {
  const bool subscribed = m_nats_client->subscribe_queue(
      subject, queue_group,
      [this, error_context, fn](const std::string &, const std::string &data,
                                const std::string &reply_to) {
        if (reply_to.empty()) {
          Logger::error("Invalid {}: missing reply subject", error_context);
          return;
        }
        try {
          m_thread_pool->enqueue(
              [this, data, reply_to, fn]() { fn(data, reply_to); });
        } catch (const std::exception &e) {
          // Pool is shutting down (enqueue throws on stopped pool). The
          // callback runs on the NATS delivery thread, so an exception must
          // never escape into the C library (undefined behavior).
          Logger::error("Failed to enqueue {} (reply_to={}): {}", error_context,
                        reply_to, e.what());
        }
      });
  return subscribed;
}

void L2Worker::run_with_nats() {
  Logger::info("Starting NATS worker mode. Subscribing to subject: {} with "
               "queue group: {}",
               m_ctx.m_config.m_nats_subject,
               m_ctx.m_config.m_nats_queue_group);

  if (m_ctx.m_config.m_db_query_enabled) {
    m_db_query_handler = std::make_unique<DbQueryHandler>();
    m_db_query_handler->set_pool_metrics(
        &m_ctx.m_worker.m_metrics->m_db_pool_connections);
  }

  RetryHandler backoff(1, 150);
  bool subscription_active = false;
  bool db_subscription_active = false;
  bool was_connected = m_nats_client && m_nats_client->is_connected();

  auto subscribe_worker = [this]() -> bool {
    Logger::info("Subscribing worker to NATS subject: {} queue group: {}",
                 m_ctx.m_config.m_nats_subject,
                 m_ctx.m_config.m_nats_queue_group);
    const bool subscribed =
        subscribe_nats_subject(m_ctx.m_config.m_nats_subject,
                               m_ctx.m_config.m_nats_queue_group,
                               "NATS request", [this](const std::string &data,
                                                      const std::string &reply_to) {
                                 process_request_from_nats(data, reply_to);
                               });
    if (!subscribed) {
      Logger::error("Failed to subscribe worker to NATS subject: {}",
                    m_ctx.m_config.m_nats_subject);
      return false;
    }
    Logger::info("Worker subscribed to NATS successfully");
    return true;
  };

  auto subscribe_db = [this]() -> bool {
    if (!m_db_query_handler || !m_db_query_handler->is_enabled()) {
      return true;
    }
    const bool subscribed =
        subscribe_nats_subject(m_ctx.m_config.m_db_query_nats_subject,
                               m_ctx.m_config.m_db_query_nats_queue_group,
                               "DB query", [this](const std::string &data,
                                                  const std::string &reply_to) {
                                 process_db_query_from_nats(data, reply_to);
                               });
    if (!subscribed) {
      Logger::error("Failed to subscribe worker to DB NATS subject: {}",
                    m_ctx.m_config.m_db_query_nats_subject);
      return false;
    }
    Logger::info("Worker subscribed to DB NATS subject: {} (queue group {})",
                 m_ctx.m_config.m_db_query_nats_subject,
                 m_ctx.m_config.m_db_query_nats_queue_group);
    return true;
  };

  while (!g_shutdown_flag) {
    const bool is_connected = m_nats_client && m_nats_client->is_connected();

    if (is_connected && !was_connected) {
      Logger::info("Worker detected restored NATS connection, forcing "
                   "subscription refresh");
      subscription_active = false;
      db_subscription_active = false;
      if (m_nats_client) {
        m_nats_client->unsubscribe();
      }
    }
    was_connected = is_connected;

    if (!m_nats_client) {
      Logger::error("NATS client is not initialized. Waiting before retry...");
    } else if (!m_nats_client->is_connected()) {
      Logger::error("Worker lost connection to NATS or NATS is unavailable. "
                    "Waiting for server recovery...");
      Logger::warn("Worker reconnect attempt in {}ms",
                   backoff.get_current_delay_ms());

      if (!m_nats_client->connect()) {
        Logger::error("Worker reconnect to NATS failed. Last error: {}",
                      m_nats_client->get_last_error().value_or(""));
      } else {
        Logger::info("Worker connected to NATS");
        backoff.record_success();
      }
    }

    if (g_shutdown_flag) {
      break;
    }

    if (m_nats_client && m_nats_client->is_connected() &&
        !subscription_active) {
      if (!subscribe_worker()) {
        Logger::warn("Subscription is not active yet, will retry in {}ms",
                     backoff.get_current_delay_ms());
      } else {
        subscription_active = true;
        backoff.record_success();
        Logger::info("NATS worker is ready and waiting for messages");
      }
    }

    // The DB gateway is independent from the worker path: databases (Oracle in
    // particular) may be cold-starting for minutes, so its retries must never
    // tear down the worker subscription (that would drop the main HTTP flow).
    // init() is incremental, so repeated calls only create the missing
    // executors; we wait until every configured database is ready before
    // subscribing.
    if (m_nats_client && m_nats_client->is_connected() &&
        subscription_active && !db_subscription_active) {
      if (!m_db_query_handler) {
        db_subscription_active = true;
      } else if (!m_db_query_handler->all_configured()) {
        m_db_query_handler->init(m_ctx.m_config.m_databases);
        if (!m_db_query_handler->all_configured()) {
          Logger::warn("DB gateway is not ready yet (database(s) "
                       "unavailable?), will retry");
          backoff.record_failure();
        } else if (!subscribe_db()) {
          Logger::warn("DB subscription failed, will retry in {}ms",
                       backoff.get_current_delay_ms());
          backoff.record_failure();
        } else {
          db_subscription_active = true;
          backoff.record_success();
        }
      } else if (!subscribe_db()) {
        Logger::warn("DB subscription failed, will retry in {}ms",
                     backoff.get_current_delay_ms());
        backoff.record_failure();
      } else {
        db_subscription_active = true;
        backoff.record_success();
      }
    }

    if (m_nats_client && !m_nats_client->is_connected()) {
      subscription_active = false;
      db_subscription_active = false;
    }

    {
      const int base_sleep_ms = backoff.get_current_delay_ms() * 200;
      const int sleep_ms = calculate_jitter_delay(base_sleep_ms, 25);
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }

    if (!subscription_active || !m_nats_client ||
        !m_nats_client->is_connected()) {
      backoff.record_failure();
    } else {
      backoff.record_success();
    }
  }

  Logger::info("NATS worker shutting down...");
  if (m_nats_client) {
    // Drain waits for in-flight messages to finish processing before closing
    m_nats_client->drain(5000);
  }
}

namespace {
// RAII helper that tracks in-flight worker requests (a gauge incremented on
// entry, decremented on exit — covering every return path) and records the
// final HTTP status code of the NATS response into the per-status family.
struct WorkerActivityGuard {
  prometheus::Gauge &m_in_flight;
  prometheus::Family<prometheus::Counter> *m_responses;
  int m_status = 500;

  WorkerActivityGuard(prometheus::Gauge &in_flight,
                      prometheus::Family<prometheus::Counter> *responses)
      : m_in_flight(in_flight), m_responses(responses) {
    m_in_flight.Increment();
  }

  ~WorkerActivityGuard() {
    m_in_flight.Decrement();
    if (m_responses != nullptr) {
      m_responses->Add({{"status", std::to_string(m_status)}}).Increment();
    }
  }
};

// Bundles the shared prologue of the main and DB NATS task handlers: the
// in-flight activity guard and the thread-local log context scope, plus the
// task start timestamp used by the tracing spans.
//
// The LogContextScope correlates all log lines of this NATS task via the
// thread-local context (each task runs on a thread pool thread).
// request_id/trace_id/client_ip are set after metadata extraction; the scope
// restores the previous values on exit so one thread cannot leak context into
// the next task.
struct WorkerTaskContext {
  WorkerActivityGuard m_activity;
  LogContextScope m_log_scope;
  uint64_t m_start_us;

  explicit WorkerTaskContext(AppContext &ctx)
      : m_activity(ctx.m_worker.m_metrics->m_in_flight_requests,
                   &ctx.m_worker.m_metrics->m_responses_total),
        m_start_us(get_current_timestamp_us()) {}
};
} // namespace

void L2Worker::process_request_from_nats(const std::string &request_json,
                                         const std::string &reply_to) {
  Logger::debug("Processing NATS request, reply_to: {}", reply_to);

  m_ctx.m_worker.m_metrics->m_bytes_received.Increment(
      static_cast<double>(request_json.size()));

  WorkerTaskContext task(m_ctx);
  update_queue_size_metric();

  const std::string nats_consume_span_id =
      JaegerSpanLogger::generate_span_id(m_ctx.m_tracer.get());
  const uint64_t start_us = task.m_start_us;

  try {
    nlohmann::json request_data;
    if (!parse_request_data(request_json, request_data)) {
      Logger::error("Failed to parse request JSON for NATS request");
      task.m_activity.m_status = 400;
      send_nats_response(reply_to,
                         make_error_json("Invalid request format").dump());
      return;
    }

    RequestData metadata = extract_request_metadata(request_data);

    Logger::set_request_id(metadata.m_request_id);
    Logger::set_client_ip(metadata.m_client_ip);
    Logger::set_trace_id(metadata.m_trace_ctx.m_trace_id);

    // Dedup: the proxy re-sends a request when it lost the reply (e.g. NATS
    // reconnect). If this request_id was already processed within the TTL
    // window, answer with the cached response instead of calling the L2 server
    // again — this gives at-most-once L2 side effects while still letting the
    // proxy complete the retried request.
    if (const auto cached_response = m_dedup_cache.find(metadata.m_request_id);
        cached_response.has_value()) {
      m_ctx.m_worker.m_metrics->m_duplicate_requests.Increment();
      Logger::warn(
          "Duplicate NATS request detected, returning cached response: "
          "request_id={} reply_to={}",
          metadata.m_request_id, reply_to);

      if (m_ctx.m_tracer && !metadata.m_trace_ctx.m_trace_id.empty()) {
        const uint64_t end_us = get_current_timestamp_us();
        log_worker_span(m_ctx.m_tracer.get(), metadata.m_method,
                        metadata.m_path, 200, start_us, end_us,
                        m_ctx.m_config.m_mode, metadata.m_request_id,
                        metadata.m_trace_ctx.m_trace_id, nats_consume_span_id,
                        metadata.m_proxy_span_id, {{"dedup.cached", true}});
      }

      task.m_activity.m_status = 200;
      send_nats_response(reply_to, *cached_response);
      record_bytes_sent(cached_response->size());
      return;
    }

    log_nats_consume_span(
        m_ctx.m_tracer.get(), metadata.m_request_id,
        metadata.m_trace_ctx.m_trace_id, start_us, nats_consume_span_id,
        metadata.m_proxy_span_id, proxy_service_name(m_ctx.m_config.m_mode),
        m_ctx.m_config.m_nats_subject, reply_to);

    const auto &worker_parent_span_id = nats_consume_span_id;

    TracingSpans spans =
        create_tracing_spans(metadata.m_trace_ctx, worker_parent_span_id);

    L2Response l2_response = execute_l2_call(metadata, spans);

    ResponseData response_data = prepare_response_data(l2_response, spans);

    task.m_activity.m_status = l2_response.m_status_code;

    HttpResponse http_response;
    http_response.m_body = l2_response.m_body;
    http_response.m_headers = l2_response.m_headers;
    http_response.m_status = l2_response.m_status_code;
    json response_headers_json = prepare_response_headers(http_response);

    std::string l2_response_stored = l2_response.m_body;
    if (response_data.m_is_binary) {
      l2_response_stored = base64::to_base64(l2_response.m_body);
    }

    const nlohmann::json response_json = build_nats_response_envelope(
        l2_response.m_status_code, metadata.m_request_id, l2_response_stored,
        response_data.m_timestamp_us, response_data.m_is_binary,
        response_data.m_content_type, response_headers_json,
        spans.m_traceparent_header);

    // Send nats_consume_span_id as NATS header instead of JSON body
    const NatsHeaders response_headers =
        make_consume_span_headers(nats_consume_span_id);

    const std::string response_json_dump = response_json.dump();
    m_dedup_cache.store(metadata.m_request_id, response_json_dump);
    send_nats_response(reply_to, response_json_dump, response_headers);

    record_bytes_sent(response_json_dump.size());

    if (m_ctx.m_tracer && !metadata.m_trace_ctx.m_trace_id.empty()) {
      const uint64_t end_us = get_current_timestamp_us();
      log_worker_span(m_ctx.m_tracer.get(), metadata.m_method,
                      metadata.m_path, l2_response.m_status_code, start_us,
                      end_us, m_ctx.m_config.m_mode, metadata.m_request_id,
                      metadata.m_trace_ctx.m_trace_id,
                      spans.m_worker_process_span_id, worker_parent_span_id);
    }

    record_l2_call_metrics(start_us);

    m_ctx.m_worker.m_metrics->m_requests_processed.Increment();

    Logger::debug("NATS request processed successfully: {}",
                  metadata.m_request_id);

  } catch (const std::exception &e) {
    Logger::error("Error processing NATS request: {}", e.what());
    task.m_activity.m_status = 500;

    nlohmann::json error_json = make_error_json("Internal server error");
    error_json["message"] = e.what();
    send_nats_response(reply_to, error_json.dump());

    record_l2_call_metrics(start_us);
  }
}

void L2Worker::send_nats_response(const std::string &reply_to,
                                  const std::string &response_json) {
  send_nats_response_impl(reply_to, response_json);
}

void L2Worker::send_nats_response(const std::string &reply_to,
                                  const std::string &response_json,
                                  const NatsHeaders &headers) {
  send_nats_response_impl(reply_to, response_json, &headers);
}

void L2Worker::send_nats_response_impl(const std::string &reply_to,
                                       const std::string &response_json,
                                       const NatsHeaders *headers) {
  constexpr int max_retries = 3;
  constexpr int retry_delay_ms = 100;

  for (int attempt = 0; attempt < max_retries; ++attempt) {
    if (!m_nats_client) {
      Logger::error("Cannot send NATS response: client not initialized");
      return;
    }

    try {
      bool success = headers ? m_nats_client->publish_with_headers(
                                   reply_to, response_json, *headers)
                             : m_nats_client->publish(reply_to, response_json);
      if (success) {
        Logger::debug("NATS response sent to: {}, size: {}", reply_to,
                      response_json.size());
        return;
      }
    } catch (const std::exception &e) {
      Logger::warn("NATS publish attempt {}/{} failed: {}", attempt + 1,
                   max_retries, e.what());
    }

    if (attempt < max_retries - 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
    }
  }

  Logger::error("NATS response delivery failed after {} attempts to: {}",
                max_retries, reply_to);
}

void L2Worker::process_db_query_from_nats(const std::string &request_json,
                                          const std::string &reply_to) {
  int status = 500;
  json body;
  std::string consume_span_id;

  WorkerTaskContext task(m_ctx);
  update_queue_size_metric();
  const uint64_t start_us = task.m_start_us;

  json request_data;
  try {
    TraceContext trace_ctx;
    const auto parsed = JsonUtils::try_parse(request_json);
    if (!parsed) {
      status = 400;
      body = make_db_error_body(status, "BAD_REQUEST", "Invalid JSON body");
    } else {
      request_data = *parsed;
      const std::string request_id =
          JsonUtils::safe_get_string(request_data, DbQueryContract::kRequestId);
      Logger::set_request_id(request_id);

      const std::string traceparent = JsonUtils::safe_get_string(
          request_data, NatsContract::kProxyTraceparent);
      trace_ctx = handle_trace_context(traceparent, m_ctx.m_tracer.get());
      Logger::set_trace_id(trace_ctx.m_trace_id);

      consume_span_id =
          JaegerSpanLogger::generate_span_id(m_ctx.m_tracer.get());
      const std::string proxy_span_id = JsonUtils::safe_get_string(
          request_data, NatsContract::kProxySpanId);

      log_nats_consume_span(
          m_ctx.m_tracer.get(), request_id, trace_ctx.m_trace_id, start_us,
          consume_span_id, proxy_span_id,
          proxy_service_name(m_ctx.m_config.m_mode),
          m_ctx.m_config.m_db_query_nats_subject, reply_to);

      if (!m_db_query_handler) {
        status = 503;
        body = make_db_error_body(status, "DB_UNAVAILABLE",
                                  "DB gateway is not initialized");
      } else {
        const uint64_t db_start_us = get_current_timestamp_us();
        m_db_query_handler->handle_request(request_data, status, body);
        const uint64_t db_end_us = get_current_timestamp_us();
        const std::string db_name = JsonUtils::safe_get_string(
            request_data, DbQueryContract::kDb);
        observe_db_request_duration(
            m_ctx.m_worker.m_metrics->m_db_query_duration_seconds,
            db_name.empty() ? "unknown" : db_name, db_start_us, db_end_us);
        if (m_ctx.m_tracer && !trace_ctx.m_trace_id.empty()) {
          const std::string db_span_id =
              JaegerSpanLogger::generate_span_id(m_ctx.m_tracer.get());
          nlohmann::json attrs = {
              {"db.name", db_name},
              {"db.operation",
               JsonUtils::safe_get_string(request_data,
                                          DbQueryContract::kType)},
          };
          log_span_to_jaeger(m_ctx.m_tracer.get(), "DB_execute",
                             "/v1/sql/" + db_name, status, db_start_us,
                             db_end_us, proxy_service_name(m_ctx.m_config.m_mode),
                             request_id, trace_ctx.m_trace_id, db_span_id,
                             consume_span_id, attrs);
        }
      }
    }
  } catch (const std::exception &e) {
    Logger::error("Error processing DB query (reply_to={}): {}", reply_to,
                  e.what());
    status = 500;
    task.m_activity.m_status = 500;
    body = make_db_error_body(status, "INTERNAL_ERROR", e.what());
  }

  task.m_activity.m_status = status;
  json envelope = make_db_response_envelope(status, body);
  const std::string db_name =
      JsonUtils::safe_get_string(request_data, DbQueryContract::kDb);
  const std::string type =
      JsonUtils::safe_get_string(request_data, DbQueryContract::kType);
  record_db_request_metrics(
      m_ctx.m_worker.m_metrics->m_db_requests_total,
      db_name.empty() ? "unknown" : db_name,
      type.empty() ? "unknown" : type, status);
  const NatsHeaders response_headers = make_consume_span_headers(consume_span_id);
  send_nats_response(reply_to, envelope.dump(), response_headers);
  record_bytes_sent(envelope.dump().size());
}

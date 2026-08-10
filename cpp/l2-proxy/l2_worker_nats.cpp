#include "common_utils.hpp"
#include "db_query_utils.hpp"
#include "json_utils.hpp"
#include "l2_worker.hpp"
#include "logger.hpp"
#include "retry_utils.hpp"
#include "time_utils.hpp"
#include "trace_logger.hpp"
#include "tracing_helpers.hpp"
#include <base64.hpp>
#include <chrono>
#include <nlohmann/json.hpp>
#include <thread>

extern std::atomic<bool> g_shutdown_flag;

void L2Worker::run_with_nats() {
  Logger::info("Starting NATS worker mode. Subscribing to subject: {} with "
               "queue group: {}",
               m_ctx.m_config.m_nats_subject,
               m_ctx.m_config.m_nats_queue_group);

  if (m_ctx.m_config.m_db_query_enabled) {
    m_db_query_handler = std::make_unique<DbQueryHandler>();
  }

  RetryHandler backoff(1, 150);
  bool subscription_active = false;
  bool db_subscription_active = false;
  bool was_connected = m_nats_client && m_nats_client->is_connected();

  auto subscribe_worker = [this]() -> bool {
    Logger::info("Subscribing worker to NATS subject: {} queue group: {}",
                 m_ctx.m_config.m_nats_subject,
                 m_ctx.m_config.m_nats_queue_group);

    const bool subscribed = m_nats_client->subscribe_queue(
        m_ctx.m_config.m_nats_subject, m_ctx.m_config.m_nats_queue_group,
        [this](const std::string &subject, const std::string &data,
               const std::string &reply_to) {
          (void)subject;

          if (reply_to.empty()) {
            Logger::error("Invalid NATS request: missing reply subject");
            return;
          }

          const auto &request_json = data;

          try {
            m_thread_pool->enqueue([this, request_json, reply_to]() {
              process_request_from_nats(request_json, reply_to);
            });
          } catch (const std::exception &e) {
            // Pool is shutting down (enqueue throws on stopped pool). The
            // callback runs on the NATS delivery thread, so an exception must
            // never escape into the C library (undefined behavior).
            Logger::error("Failed to enqueue NATS request (reply_to={}): {}",
                          reply_to, e.what());
          }
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
    const bool subscribed = m_nats_client->subscribe_queue(
        m_ctx.m_config.m_db_query_nats_subject,
        m_ctx.m_config.m_db_query_nats_queue_group,
        [this](const std::string &subject, const std::string &data,
               const std::string &reply_to) {
          (void)subject;
          if (reply_to.empty()) {
            Logger::error("Invalid DB query request: missing reply subject");
            return;
          }
          try {
            m_thread_pool->enqueue([this, data, reply_to]() {
              process_db_query_from_nats(data, reply_to);
            });
          } catch (const std::exception &e) {
            Logger::error("Failed to enqueue DB query (reply_to={}): {}",
                          reply_to, e.what());
          }
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

void L2Worker::process_request_from_nats(const std::string &request_json,
                                         const std::string &reply_to) {
  Logger::debug("Processing NATS request, reply_to: {}", reply_to);

  m_ctx.m_worker.m_metrics->m_bytes_received.Increment(
      static_cast<double>(request_json.size()));

  const std::string nats_consume_span_id =
      JaegerSpanLogger::generate_span_id(m_ctx.m_tracer.get());
  const uint64_t start_us = get_current_timestamp_us();

  // Correlate all log lines of this NATS task via the thread-local context
  // (each task runs on a thread pool thread). request_id/trace_id/client_ip
  // are set after metadata extraction; the scope restores the previous values
  // on exit so one thread cannot leak context into the next task.
  LogContextScope log_scope;

  try {
    nlohmann::json request_data;
    if (!parse_request_data(request_json, request_data)) {
      Logger::error("Failed to parse request JSON for NATS request");
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
        log_span_to_jaeger(
            m_ctx.m_tracer.get(), metadata.m_method, metadata.m_path, 200,
            start_us, end_us, proxy_service_name(m_ctx.m_config.m_mode),
            metadata.m_request_id, metadata.m_trace_ctx.m_trace_id,
            nats_consume_span_id, metadata.m_proxy_span_id,
            {{"dedup.cached", true}});
      }

      send_nats_response(reply_to, *cached_response);
      m_ctx.m_worker.m_metrics->m_bytes_sent.Increment(
          static_cast<double>(cached_response->size()));
      return;
    }

    if (m_ctx.m_tracer && !metadata.m_trace_ctx.m_trace_id.empty()) {
      nlohmann::json attrs = {
          {"messaging.system", "nats"},
          {"messaging.operation", "consume"},
          {"messaging.destination", m_ctx.m_config.m_nats_subject},
          {"messaging.reply_to", reply_to}};
      log_span_to_jaeger(m_ctx.m_tracer.get(), "NATS_consume", "/nats", 200,
                         start_us, start_us,
                         proxy_service_name(m_ctx.m_config.m_mode),
                         metadata.m_request_id, metadata.m_trace_ctx.m_trace_id,
                         nats_consume_span_id, metadata.m_proxy_span_id);
    }

    const auto &worker_parent_span_id = nats_consume_span_id;

    TracingSpans spans =
        create_tracing_spans(metadata.m_trace_ctx, worker_parent_span_id);

    L2Response l2_response = execute_l2_call(metadata, spans);

    ResponseData response_data = prepare_response_data(l2_response, spans);

    nlohmann::json response_json;
    response_json[NatsResponseContract::kStatus] = l2_response.m_status_code;

    HttpResponse http_response;
    http_response.m_body = l2_response.m_body;
    http_response.m_headers = l2_response.m_headers;
    http_response.m_status = l2_response.m_status_code;
    json response_headers_json = prepare_response_headers(http_response);
    if (!response_headers_json.empty()) {
      response_json[NatsResponseContract::kHeaders] = response_headers_json;
    }

    std::string l2_response_stored = l2_response.m_body;
    if (response_data.m_is_binary) {
      l2_response_stored = base64::to_base64(l2_response.m_body);
    }

    response_json[NatsResponseContract::kBody]
                 [NatsResponseContract::kBodyRequestId] = metadata.m_request_id;
    response_json[NatsResponseContract::kBody]
                 [NatsResponseContract::kBodyResponse] = l2_response_stored;
    response_json[NatsResponseContract::kBody]
                 [NatsResponseContract::kBodyTimestamp] =
                     response_data.m_timestamp_us;
    response_json[NatsResponseContract::kBody]
                 [NatsResponseContract::kBodyIsBinary] =
                     nlohmann::json(response_data.m_is_binary);
    response_json[NatsResponseContract::kBody]
                 [NatsResponseContract::kBodyContentType] =
                     response_data.m_content_type;

    if (!spans.m_traceparent_header.empty()) {
      response_json[NatsResponseContract::kBody]
                   [NatsResponseContract::kBodyTraceparent] =
                       spans.m_traceparent_header;
    }

    // Send nats_consume_span_id as NATS header instead of JSON body
    NatsHeaders response_headers;
    if (!nats_consume_span_id.empty()) {
      response_headers[NatsContract::kConsumeSpanIdHeader] =
          nats_consume_span_id;
    }

    const std::string response_json_dump = response_json.dump();
    m_dedup_cache.store(metadata.m_request_id, response_json_dump);
    send_nats_response(reply_to, response_json_dump, response_headers);

    m_ctx.m_worker.m_metrics->m_bytes_sent.Increment(
        static_cast<double>(response_json_dump.size()));

    if (m_ctx.m_tracer && !metadata.m_trace_ctx.m_trace_id.empty()) {
      const uint64_t end_us = get_current_timestamp_us();
      log_span_to_jaeger(m_ctx.m_tracer.get(), metadata.m_method,
                         metadata.m_path, l2_response.m_status_code, start_us,
                         end_us, proxy_service_name(m_ctx.m_config.m_mode),
                         metadata.m_request_id, metadata.m_trace_ctx.m_trace_id,
                         spans.m_worker_process_span_id, worker_parent_span_id);
    }

    record_l2_call_metrics(start_us);

    m_ctx.m_worker.m_metrics->m_requests_processed.Increment();

    Logger::debug("NATS request processed successfully: {}",
                  metadata.m_request_id);

  } catch (const std::exception &e) {
    Logger::error("Error processing NATS request: {}", e.what());

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
  const uint64_t start_us = get_current_timestamp_us();

  // Correlate all DB gateway log lines of this NATS task via the thread-local
  // context; the scope restores the previous values on exit so one thread
  // cannot leak context into the next task.
  LogContextScope log_scope;

  json request_data;
  TraceContext trace_ctx;
  try {
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

      if (m_ctx.m_tracer && !trace_ctx.m_trace_id.empty()) {
        nlohmann::json attrs = {
            {"messaging.system", "nats"},
            {"messaging.operation", "consume"},
            {"messaging.destination", m_ctx.m_config.m_db_query_nats_subject},
            {"messaging.reply_to", reply_to},
        };
        log_span_to_jaeger(m_ctx.m_tracer.get(), "NATS_consume", "/nats", 200,
                           start_us, start_us,
                           proxy_service_name(m_ctx.m_config.m_mode),
                           request_id, trace_ctx.m_trace_id, consume_span_id,
                           proxy_span_id, attrs);
      }

      if (!m_db_query_handler) {
        status = 503;
        body = make_db_error_body(status, "DB_UNAVAILABLE",
                                  "DB gateway is not initialized");
      } else {
        const uint64_t db_start_us = get_current_timestamp_us();
        m_db_query_handler->handle_request(request_data, status, body);
        const uint64_t db_end_us = get_current_timestamp_us();
        if (m_ctx.m_tracer && !trace_ctx.m_trace_id.empty()) {
          const std::string db_span_id =
              JaegerSpanLogger::generate_span_id(m_ctx.m_tracer.get());
          const std::string db_name = JsonUtils::safe_get_string(
              request_data, DbQueryContract::kDb);
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
    body = make_db_error_body(status, "INTERNAL_ERROR", e.what());
  }

  json envelope{{DbQueryContract::kStatus, status},
                {DbQueryContract::kBody, body}};
  NatsHeaders response_headers;
  if (!consume_span_id.empty()) {
    response_headers[NatsContract::kConsumeSpanIdHeader] = consume_span_id;
  }
  send_nats_response(reply_to, envelope.dump(), response_headers);
  m_ctx.m_worker.m_metrics->m_bytes_sent.Increment(
      static_cast<double>(envelope.dump().size()));
}

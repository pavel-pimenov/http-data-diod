#include "l2_worker.hpp"
#include "app_context.hpp"
#include "constants.hpp"
#include "error_types.hpp"
#include "header_utils.hpp"
#include "http_client_pool.hpp"
#include "json_schema_validator.hpp"
#include "json_utils.hpp"
#include "l2_routing.hpp"
#include "metrics_manager.hpp"
#include "nats_client.hpp"
#include "pool_executor.hpp"
#include "time_utils.hpp"
#include "worker_request_parser.hpp"
#include <base64.hpp>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <nlohmann/json.hpp>
#include <stop_token>
#include <stdexcept>
#include <utility>
#if __has_include(<generator>)
#include <generator>
#endif

#include "common_utils.hpp"
#include "http_client.hpp"
#include "logger.hpp"
#include "retry_utils.hpp"
#include "sentry_client.hpp"
#include "thread_pool_wrapper.hpp"
#include "trace_logger.hpp"
#include "tracing_helpers.hpp"
#include <chrono>
#include <tuple>
#include <cstdlib>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string_view>
#include <thread>

L2Worker::L2Worker(AppContext &context)
    // Initialize HTTP client pool with optimized settings
    // Parameters: max_pool_size, timeout_seconds, acquire_timeout_seconds,
    // enable_connection_reuse
    : m_clients{.m_http_client_pool = std::make_unique<HttpClientPool>(
                    context.m_config.m_proxy.m_http_pool_size,       // max connections
                    context.m_config.m_proxy.m_http_timeout_seconds, // request timeout
                    http_timeouts::kPoolAcquireTimeoutSeconds, // acquire
                    true, // enable connection reuse (keep-alive)
                    context.m_config.m_ssl.m_enable_server_certificate_verification,
                    context.m_config.m_ssl.m_enable_server_hostname_verification,
                    context.m_config.m_ssl.m_ca_cert_path,
                    context.m_config.m_proxy.m_http_pool_idle_timeout_seconds)},
      m_ctx(context), m_l2_server_urls(context.m_config.m_server.m_urls),
m_dedup_cache(context.m_config.m_dedup.m_enabled,
                  context.m_config.m_dedup.m_max_entries,
                  context.m_config.m_dedup.m_ttl_ms) {

  if (context.m_proxy.m_http_pool_metrics) {
    const auto &metrics = *context.m_proxy.m_http_pool_metrics;
    m_clients.m_http_client_pool->set_metrics(
        PoolMetrics{metrics.m_active_clients, metrics.m_available_clients,
                    metrics.m_client_acquisitions_total,
                    metrics.m_client_releases_total,
                    metrics.m_acquisition_timeouts_total,
                    metrics.m_acquisition_duration_seconds,
                    metrics.m_stale_evictions_total});
  }

  Logger::info("Initializing NATS client for subject: {}, queue group: {}",
               context.m_config.m_nats.m_subject,
               context.m_config.m_nats.m_queue_group);

  m_clients.m_nats_client =
      std::make_unique<NatsClient>(context.m_config.create_nats_config());

  if (!m_clients.m_nats_client->connect()) {
    Logger::error("Failed to connect to NATS server. Worker is configured for "
                  "NATS only.");
  } else {
    Logger::info("NATS client connected successfully");
  }

  // Initialize thread pool with more worker threads for better concurrency.
  // NONE (inline execution) would run the entire NATS handler - including
  // blocking L2 HTTP calls - on the NATS delivery thread and stall the broker.
  // Force CUSTOM for worker/NATS mode even if config requests NONE.
  ThreadPoolWrapper::Type pool_type;
  if (m_ctx.m_config.m_app.m_thread_pool_type == "none") {
    if (m_ctx.m_config.m_app.m_mode == "worker") {
      Logger::warn(
          "THREAD_POOL_TYPE=none is unsafe for NATS worker (blocks delivery "
          "thread); forcing custom pool (threads={}, queue={})",
          m_ctx.m_config.m_worker.m_threads,
          m_ctx.m_config.m_worker.m_queue_size);
      pool_type = ThreadPoolWrapper::Type::CUSTOM;
    } else {
      pool_type = ThreadPoolWrapper::Type::NONE;
    }
  } else {
    pool_type = ThreadPoolWrapper::Type::CUSTOM;
  }
  m_clients.m_thread_pool = std::make_unique<ThreadPoolWrapper>(
      pool_type, m_ctx.m_config.m_worker.m_threads,
      static_cast<size_t>(m_ctx.m_config.m_worker.m_queue_size));

  // The queue depth is only a saturation signal together with its capacity:
  // the limit is either configured explicitly or derived from the thread count.
  m_ctx.m_worker.m_metrics->m_queue_capacity.Set(
      static_cast<double>(m_clients.m_thread_pool->max_queue_capacity()));

  m_circuit_breaker.set_gauge(m_ctx.m_worker.m_metrics->m_circuit_breaker_state);
  m_circuit_breaker.set_counters(
      m_ctx.m_worker.m_metrics->m_circuit_breaker_transitions_total,
      m_ctx.m_worker.m_metrics->m_circuit_breaker_rejected_total);

  Logger::info("L2Worker initialized successfully");
}

L2Worker::~L2Worker() {
  try {
    Logger::info("L2Worker shutting down pools...");

    // Shutdown thread pool first to prevent new tasks
    if (m_clients.m_thread_pool) {
      Logger::debug("Shutting down thread pool...");
      m_clients.m_thread_pool.reset();
    }

    if (m_clients.m_http_client_pool) {
      Logger::debug("Shutting down HTTP client pool...");
      m_clients.m_http_client_pool.reset();
    }

    Logger::info("L2Worker shutdown complete");
    // NOLINTNEXTLINE(bugprone-empty-catch) — destructor must not throw
  } catch (...) {
  }
}

std::string
L2Worker::extract_l2_server_span_id(const std::string &l2_response) {
  std::string l2_server_span_id;
  try {
    const auto l2_resp_result = JsonUtils::try_parse(l2_response);
    if (l2_resp_result &&
        JsonUtils::has_key(*l2_resp_result, "server_span_id")) {
      l2_server_span_id =
          JsonUtils::safe_get_string(*l2_resp_result, "server_span_id");
      Logger::debug("Worker extracted l2_server_span_id: {}",
                    l2_server_span_id);
    } else {
      // server_span_id is optional - not all L2 server implementations return
      // it
      Logger::debug("Worker: l2_response does not contain server_span_id "
                    "(optional field)");
    }
  } catch (const std::exception &e) {
    // Non-critical: tracing will still work without server_span_id
    Logger::debug("Worker: Could not parse l2_response or extract "
                  "server_span_id (non-critical): {}",
                  e.what());
  }
  return l2_server_span_id;
}

json L2Worker::prepare_response_headers(const HttpResponse &l2_http_response) {
  return HeaderUtils::headers_to_json(l2_http_response.m_headers);
}

bool L2Worker::validate_l2_server_access(const std::string &path,
                                         std::string &selected_url) {
  if (l2_routing::find_allowed_l2_server(m_l2_server_urls, path,
                                         selected_url)) {
    return true;
  }
  if (m_l2_server_urls.empty()) {
    Logger::error("{}", "No L2 server URLs configured - add L2_SERVER_URLS");
    return false;
  }
  Logger::error("Access to arbitrary L2 server address is forbidden: "
                "attempted path = {} L2_SERVER_URLS = {}",
                path, selected_url);
  return false;
}

HttpResponse L2Worker::call_l2_server(
    const std::string &path, const std::string &body,
    const std::string &traceparent, const httplib::Headers &forwarded_headers,
    const std::string &method, const std::string &parent_span_id,
    const std::string &l2_call_span_id, const std::string &query) {
  const auto l2_call_profiler = create_scoped_request_profiler(
      m_ctx.m_worker.m_metrics->m_l2_call_duration_seconds);
  m_ctx.m_worker.m_metrics->m_l2_calls.Increment();

  std::string selected_url;
  if (!validate_l2_server_access(path, selected_url)) {
    return make_error_response(
        http_status::kForbidden,
        "Access to arbitrary L2 server address is forbidden! path=" + path);
  }

  Logger::debug("Calling L2 server: selected_url={}", selected_url);
  Logger::debug("Calling L2 server: path={} body_size={} traceparent={}", path,
                body.size(), traceparent);

  if (!forwarded_headers.empty()) {
    Logger::debug("Worker sending {} forwarded headers to L2 server",
                  forwarded_headers.size());
  }

  const TraceContext l2_trace_ctx =
      handle_trace_context(traceparent, m_ctx.m_tracer.get());
  // Use the provided l2_call_span_id if available, otherwise generate one.
  // Rebuild the traceparent sent to the L2 server so the span id it links to
  // matches the span we log below. handle_trace_context generates a fresh
  // span id, which would orphan the L2 server span (parent not in trace).
  const auto [actual_l2_call_span_id, traceparent_result] =
      make_span_and_traceparent(m_ctx.m_tracer.get(), l2_trace_ctx,
                                l2_call_span_id, l2_trace_ctx.m_sampled);

  const auto start_us = get_current_timestamp_us();

  // url is path-only: query string is appended only for the actual HTTP call
  // so that INFO logs and Jaeger http.url stay free of query parameters
  // (they may contain credentials/tokens).
  const auto url = l2_routing::build_l2_url(selected_url, path);
  Logger::debug("Calling L2 server: URL={}", url);

  int final_attempt = 0;
  const auto backend_call_started_at = std::chrono::steady_clock::now();
  HttpResponse http_response =
      execute_l2_call_with_retry(url, query, body, traceparent_result,
                                 forwarded_headers, method, final_attempt);
  // The aggregate histogram above cannot tell which backend is slow when
  // several L2_SERVER_URLS are configured, so the same duration is observed
  // per backend (the label is the configured base URL — a bounded set).
  const auto backend_call_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                     backend_call_started_at)
          .count();
  m_ctx.m_worker.m_metrics->m_l2_server_call_duration_seconds
      .Add({{"backend", selected_url}}, latency_buckets_5ms_to_10s())
      .Observe(backend_call_seconds);
  // 5xx from L2 is a server failure (circuit-breaker/retry signal) even
  // when the response carries headers (e.g. JSON error). 4xx is client error
  // and does not trip the breaker.
  const bool success = http_response.m_status < http_status::kServerErrorMin;

  if (m_ctx.m_tracer && !l2_trace_ctx.m_trace_id.empty()) {
    const auto end_us = get_current_timestamp_us();
    const int span_status =
        success ? http_response.m_status : http_status::kInternalServerError;
    nlohmann::json attrs;
    if (!success) {
      // Expose how many L2 server attempts were made before giving up so the
      // retry behaviour is visible in traces (l2_call span status = 500).
      attrs["l2_call.attempts"] = final_attempt;
    }
    JaegerSpanLogger::log_l2_call(
        m_ctx.m_tracer.get(), method, url, span_status, start_us, end_us,
        l2_trace_ctx, m_ctx.m_config.m_app.m_mode, actual_l2_call_span_id,
        resolve_parent_id(parent_span_id, l2_trace_ctx.m_parent_id), "", attrs);
  }

  return http_response;
}

void L2Worker::run() {
  Logger::info("C++ L2 Worker started. Waiting for requests...");

  // Sample pool saturation in the background so the queue-depth gauge is not
  // flat between requests. jthread auto-joins and respects stop_token.
  m_state.m_metrics_ticker = std::jthread([this](const std::stop_token &st) {
    metrics_ticker_loop(st);
  });

  run_with_nats();

  if (m_state.m_metrics_ticker.joinable()) {
    m_state.m_metrics_ticker.request_stop();
    m_state.m_metrics_ticker.join();
  }

  Logger::info("Shutting down gracefully...");

  // Graceful-shutdown gauge: drain duration from the shutdown flag (signal)
  // to the moment all in-flight tasks have completed. 0 while running.
  const auto drain_start = std::chrono::steady_clock::now();

  // Thread pool destructor drains all in-flight tasks before joining
  Logger::info("Waiting for in-flight requests to complete...");
  if (m_clients.m_thread_pool) {
    m_clients.m_thread_pool.reset();
  }

  const auto drain_seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - drain_start);
  m_ctx.m_worker.m_metrics->m_graceful_shutdown_seconds.Set(
      drain_seconds.count());
  Logger::info("Graceful shutdown completed in {:.2f}s",
               drain_seconds.count());
}

void L2Worker::update_queue_size_metric() const {
  if (m_clients.m_thread_pool) {
    m_ctx.m_worker.m_metrics->m_queue_size.Set(
        static_cast<double>(m_clients.m_thread_pool->queue_size()));
    m_ctx.m_worker.m_metrics->m_active_workers.Set(
        static_cast<double>(m_clients.m_thread_pool->active_count()));
  }
}

void L2Worker::record_bytes_sent(size_t bytes) const {
  m_ctx.m_worker.m_metrics->m_bytes_sent.Increment(
      static_cast<double>(bytes));
}

void L2Worker::metrics_ticker_loop(const std::stop_token &st) {
  constexpr auto kStep = std::chrono::milliseconds(100);
  constexpr int kStepsPerSample = 50; // ~5s between samples
  int step_count = 0;
  while (!st.stop_requested()) {
    if (++step_count >= kStepsPerSample) {
      step_count = 0;
      update_queue_size_metric();
    }
    std::this_thread::sleep_for(kStep);
  }
}

bool L2Worker::is_nats_connected() const {
  return m_clients.m_nats_client && m_clients.m_nats_client->is_connected();
}

#if __has_include(<generator>)
// C++23 generator: lazy sequence of attempt numbers (1..max), co_yield
// demonstrates coroutine-based backpressure — no vector allocation, frame
// suspends between attempts, composes with ranges::filter/take.
std::generator<int> attempt_sequence(int max) {
  for (int i = 1; i <= max; ++i) {
    co_yield i;
  }
  co_return;
}
#endif

void L2Worker::count_retry(const char *outcome) const {
  m_ctx.m_worker.m_metrics->m_retries_total.Add({{"outcome", outcome}}).Increment();
}

HttpResponse L2Worker::execute_l2_call_with_retry(
    const std::string &url, const std::string &query, const std::string &body,
    const std::string &traceparent, const httplib::Headers &forwarded_headers,
    const std::string &method, int &final_attempt) {
  // Check circuit breaker before attempting calls
  if (!m_circuit_breaker.allow_request()) {
    Logger::warn(
        "L2 server circuit breaker is OPEN for url={}, rejecting request "
        "(state={})",
        url, m_circuit_breaker.state_name());
    return make_error_response(
        http_status::kServiceUnavailable,
        "L2 server circuit breaker open, service temporarily unavailable");
  }

  const int max_retries = m_ctx.m_config.m_proxy.m_max_retries;
  std::runtime_error last_error("Initial error");

#if __has_include(<generator>)
  for (int attempt : attempt_sequence(max_retries)) {
#else
  for (int attempt = 1; attempt <= max_retries; ++attempt) {
#endif
    final_attempt = attempt;
    try {
      const auto [http_response, http_code] = execute_http_command_with_status(
          m_clients.m_http_client_pool.get(),
          [&url, &query, &body, &traceparent, &forwarded_headers,
           &method](HttpClient *client) {
            // Append the query string only to the request URL; the `url`
            // variable (used for logging) stays clean.
            auto request_url = url;
            if (!query.empty()) {
              request_url += "?";
              request_url += query;
            }
            if (method == "POST") {
              return client->post(request_url, body, traceparent,
                                  forwarded_headers);
            }
            return client->get(request_url, traceparent, forwarded_headers);
          });

      size_t response_size = http_response.m_body.size();

      const auto x_real_ip =
          get_header_value(forwarded_headers, "X-Real-IP");
      const auto x_datahub_client_id =
          get_header_value(forwarded_headers, "X-DataHub-Client-Id");
      const auto user_agent =
          shorten_user_agent(get_header_value(forwarded_headers, "User-Agent"));

      Logger::info("L2 server call completed: method={} url={} status={} "
                   "response_size={} bytes request_size={} x_real_ip={} "
                   "x_datahub_client_id={} user_agent={}",
                   method, url, http_code, response_size, body.size(),
                   x_real_ip, x_datahub_client_id, user_agent);

      if ((http_code == http_status::kBadGateway ||
           http_code == http_status::kServiceUnavailable ||
           http_code == http_status::kGatewayTimeout) &&
          attempt < max_retries) {
        Logger::warn("L2 server returned {} on attempt {} url: {}. Retrying...",
                     http_code, attempt, url);
        count_retry("retried");
        sleep_for_attempt_jitter(attempt);
        continue;
      }
      if (http_code >= http_status::kServerErrorMin && attempt >= max_retries) {
        count_retry("exhausted");
      }

      if (http_code >= http_status::kSuccessMin &&
          http_code < http_status::kRedirectMax) {
        m_circuit_breaker.record_success();
      } else {
        m_circuit_breaker.record_failure();
      }
      return http_response;
    } catch (const std::runtime_error &e) {
      last_error = e;
      if (attempt < max_retries) {
        Logger::warn(
            "L2 server call failed on attempt {}: {} url: {}. Retrying...",
            attempt, e.what(), url);
        count_retry("retried");
        sleep_for_attempt_jitter(attempt);
      } else {
        count_retry("exhausted");
        handle_http_error(std::string(e.what()),
                          &m_ctx.m_worker.m_metrics->m_l2_errors,
                          "L2 server call", max_retries, url);
        m_ctx.m_worker.m_metrics->m_l2_call_errors_total
            .Add({{"type", l2_error_type_to_string(
                              categorize_l2_error(std::string(e.what())))}})
            .Increment();
        if (m_ctx.m_sentry) {
          m_ctx.m_sentry->capture_message(
              std::format(
                  "L2 server call failed after {} attempts: {} url: {}",
                  max_retries, e.what(), url),
              "", {"l2_server_call_error", url});
        }
      }
    }
  }

  m_circuit_breaker.record_failure();
  return make_error_response(
      http_status::kInternalServerError,
      std::format("Failed to call L2 server ( url: {} ) after {} attempts: {}",
                  url, max_retries, last_error.what()));
}

void L2Worker::record_l2_call_metrics(uint64_t start_us) const {
  const uint64_t end_us = get_current_timestamp_us();
  const double duration_seconds = TimeUtils::duration_seconds(start_us, end_us);
  try {
    m_ctx.m_worker.m_metrics->m_request_duration_seconds.Observe(
        duration_seconds);
  } catch (const std::exception &e) {
    Logger::error("Failed to record request duration histogram: {}", e.what());
  } catch (...) {
    Logger::error(
        "Failed to record request duration histogram with unknown error");
  }
}


// ============================================================================
// Pipeline Stage Implementations for NATS request processing
// ============================================================================

bool L2Worker::parse_request_data(const std::string &request_json,
                                  json &request_data) const {
  // C++23 expected monadic: and_then chains JSON parse → schema validation
  auto validated = validate_and_parse_json(request_json, "Worker")
                       .and_then([&](json j) -> std::expected<json, std::string> {
                         std::string parse_error;
                         std::string failed_field;
                         static RequestValidator const g_validator =
                             create_standard_request_validator();
                         if (!g_validator.validate(j, parse_error,
                                                   &failed_field)) {
                           Logger::error("Worker request validation failed: {}",
                                         parse_error);
                           handle_processing_error_with_category(
                               parse_error,
                               ProcessingErrorMetrics{
                                   .m_total_errors = nullptr,
                                   .m_json_errors =
                                       &m_ctx.m_worker.m_metrics->m_processing_json_errors,
                                   .m_validation_errors =
                                       &m_ctx.m_worker.m_metrics
                                            ->m_processing_validation_errors},
                               "Request Schema Validation");
                           // Which field the client got wrong: the reason text
                           // cannot be a label (it embeds the client's own
                           // path and method).
                           m_ctx.m_worker.m_metrics->m_request_validation_errors
                               .Add({{"field", failed_field}})
                               .Increment();
                           if (m_ctx.m_sentry) {
                             m_ctx.m_sentry->capture_message(
                                 std::format(
                                     "Worker request validation failed: {}",
                                     parse_error),
                                 j.value(NatsContract::kRequestId,
                                         std::string{}),
                                 {"worker_validation_error", "schema"});
                           }
                           return std::unexpected(parse_error);
                         }
                         return j;
                       });
  if (!validated) {
    return false;
  }
  request_data = std::move(*validated);

  const std::string method = request_data[NatsContract::kMethod];
  if (method != "POST" && method != "GET") {
    Logger::error("Skipping non-POST request: {}", method);
    return false;
  }

  return true;
}

L2Worker::RequestData
L2Worker::extract_request_metadata(const json &request_data) const {
  RequestData metadata;
  // Fill the shared-contract fields in one move instead of copying them
  // field by field; the base subobject is assigned in place.
  static_cast<worker_request_parser::WorkerRequestData &>(metadata) =
      worker_request_parser::decode_nats_request(request_data);

  const auto effective_traceparent =
      !metadata.m_proxy_traceparent.empty() ? metadata.m_proxy_traceparent
                                            : metadata.m_traceparent;
  metadata.m_trace_ctx =
      handle_trace_context(effective_traceparent, m_ctx.m_tracer.get());

  if (request_data.contains(NatsContract::kHeaders)) {
    Logger::debug(
        "Worker extracting forwarded headers from backend request: {} headers",
        request_data[NatsContract::kHeaders].size());
  }

  return metadata;
}

L2Worker::TracingSpans
L2Worker::create_tracing_spans(const TraceContext &parent_trace_ctx,
                               const std::string &op_parent_span_id) const {
  TracingSpans spans;

  spans.m_worker_process_parent_id = op_parent_span_id;

  if (m_ctx.m_tracer && !parent_trace_ctx.m_trace_id.empty()) {
    spans.m_worker_process_span_id = JaegerLogger::generate_span_id();
    Logger::debug("Worker processing span created: {} parent: {}",
                  spans.m_worker_process_span_id,
                  spans.m_worker_process_parent_id);

    std::tie(spans.m_l2_call_span_id, spans.m_traceparent_header) =
        make_span_and_traceparent(m_ctx.m_tracer.get(), parent_trace_ctx, "",
                                  true);
    Logger::debug("Worker l2_call_span_id: {} traceparent_header: {}",
                  spans.m_l2_call_span_id, spans.m_traceparent_header);
  }

  return spans;
}

L2Worker::L2Response L2Worker::execute_l2_call(const RequestData &metadata,
                                               const TracingSpans &spans) {
  L2Response response;

  Logger::debug("Worker calling L2 server for request_id={}",
                metadata.m_request_id);
  const HttpResponse l2_http_response =
      call_l2_server(metadata.m_path, metadata.m_body,
                     spans.m_traceparent_header, metadata.m_forwarded_headers,
                     metadata.m_method, spans.m_worker_process_span_id,
                     spans.m_l2_call_span_id, metadata.m_query);

  response.m_body = l2_http_response.m_body;
  response.m_headers = l2_http_response.m_headers;
  response.m_status_code = l2_http_response.m_status;
  response.m_l2_server_span_id = extract_l2_server_span_id(response.m_body);

  // Record L2 response size in histogram
  m_ctx.m_worker.m_metrics->m_l2_response_size_bytes.Observe(
      static_cast<double>(l2_http_response.m_body.size()));

  Logger::debug("Worker received response from L2 server for request_id={}",
                metadata.m_request_id);

  return response;
}

L2Worker::ResponseData
L2Worker::prepare_response_data(const L2Response &l2_response,
                                const TracingSpans &spans) {
  ResponseData data;

  data.m_timestamp_us = TimeUtils::epoch_us();

  const auto content_type_it = l2_response.m_headers.find("Content-Type");
  data.m_content_type = (content_type_it != l2_response.m_headers.end())
                            ? content_type_it->second
                            : "";
  data.m_is_binary = HeaderUtils::is_binary_content_type(data.m_content_type);

  Logger::debug("Worker checking response type: content_type={} is_binary={}",
                data.m_content_type, data.m_is_binary);

  return data;
}

#include "l2_worker.hpp"
#include "header_utils.hpp"
#include "json_schema_validator.hpp"
#include "json_utils.hpp"
#include "time_utils.hpp"
#include <base64.hpp>
#include <cstring>
#include <format>
#include <nlohmann/json.hpp>

#include "common_utils.hpp"
#include "http_client.hpp"
#include "logger.hpp"
#include "retry_utils.hpp"
#include "scoped_profiler.hpp"
#include "thread_pool_wrapper.hpp"
#include "trace_logger.hpp"
#include "tracing_helpers.hpp"
#include <algorithm>
#include <chrono>
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
    : m_http_client_pool(std::make_unique<HttpClientPool>(
          context.m_config.m_http_pool_size,       // max connections
          context.m_config.m_http_timeout_seconds, // request timeout
          30,   // acquire timeout (30 seconds)
          true, // enable connection reuse (keep-alive)
          context.m_config.m_enable_ssl_server_certificate_verification,
          context.m_config.m_enable_ssl_server_hostname_verification,
          context.m_config.m_ssl_ca_cert_path,
          context.m_config.m_http_pool_idle_timeout_seconds)),
      m_ctx(context), m_l2_server_urls(context.m_config.m_l2_server_urls),
      m_dedup_cache(context.m_config.m_dedup_enabled,
                    context.m_config.m_dedup_max_entries,
                    context.m_config.m_dedup_ttl_ms) {

  if (context.m_proxy.m_http_pool_metrics) {
    m_http_client_pool->set_metrics(
        &context.m_proxy.m_http_pool_metrics->m_active_clients,
        &context.m_proxy.m_http_pool_metrics->m_available_clients,
        &context.m_proxy.m_http_pool_metrics->m_client_acquisitions_total,
        &context.m_proxy.m_http_pool_metrics->m_client_releases_total,
        nullptr, // acquisition_timeouts (not tracked for worker pool)
        nullptr, // acquisition_duration (not tracked for worker pool)
        &context.m_proxy.m_http_pool_metrics->m_stale_evictions_total);
  }

  Logger::info("Initializing NATS client for subject: {}, queue group: {}",
               context.m_config.m_nats_subject,
               context.m_config.m_nats_queue_group);

  m_nats_client =
      std::make_unique<NatsClient>(context.m_config.create_nats_config());

  if (!m_nats_client->connect()) {
    Logger::error("Failed to connect to NATS server. Worker is configured for "
                  "NATS only.");
  } else {
    Logger::info("NATS client connected successfully");
  }

  // Initialize thread pool with more worker threads for better concurrency
  ThreadPoolWrapper::Type pool_type;
  if (m_ctx.m_config.m_thread_pool_type == "none") {
    pool_type = ThreadPoolWrapper::Type::NONE;
  } else {
    pool_type = ThreadPoolWrapper::Type::CUSTOM;
  }
  m_thread_pool = std::make_unique<ThreadPoolWrapper>(
      pool_type, m_ctx.m_config.m_l2_worker_threads,
      static_cast<size_t>(m_ctx.m_config.m_l2_worker_queue_size));

  m_circuit_breaker.set_gauge(
      &m_ctx.m_worker.m_metrics->m_circuit_breaker_state);

  Logger::info("L2Worker initialized successfully");
}

L2Worker::~L2Worker() {
  try {
    Logger::info("L2Worker shutting down pools...");

    // Shutdown thread pool first to prevent new tasks
    if (m_thread_pool) {
      Logger::debug("Shutting down thread pool...");
      m_thread_pool.reset();
    }

    if (m_http_client_pool) {
      Logger::debug("Shutting down HTTP client pool...");
      m_http_client_pool.reset();
    }

    Logger::info("L2Worker shutdown complete");
    // NOLINTNEXTLINE(bugprone-empty-catch) — destructor must not throw
  } catch (...) {
  }
}

std::string L2Worker::extract_scheme_host_port(const std::string &url) {
  size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos)
    return "";

  std::string scheme = url.substr(0, scheme_end);
  std::string rest = url.substr(scheme_end + 3);

  size_t path_start = rest.find('/');
  std::string host_port =
      (path_start == std::string::npos) ? rest : rest.substr(0, path_start);

  return scheme + "://" + host_port;
}

void L2Worker::extract_forwarded_headers(const json &request_data,
                                         httplib::Headers &forwarded_headers) {
  if (request_data.contains(NatsContract::kHeaders)) {
    Logger::debug(
        "Worker extracting forwarded headers from backend request: {} headers",
        request_data[NatsContract::kHeaders].size());
    HeaderUtils::filter_headers_from_json(
        request_data[NatsContract::kHeaders], forwarded_headers,
        HeaderUtils::get_default_skip_headers(), "Worker");
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

bool L2Worker::is_l2_server_allowed(const std::string &path,
                                    std::string &selected_url) {
  if (m_l2_server_urls.empty()) {
    Logger::error("{}", "No L2 server URLs configured - add L2_SERVER_URLS");
    return false;
  }
  // Allow common paths that are safe to proxy
  if (path == "/metrics" || path == "/" || path == "/favicon.ico") {
    // Use the first configured URL for common paths
    selected_url = m_l2_server_urls[0];
    return true;
  }

  bool allowed = false;
  for (const auto &allowed_base : m_l2_server_urls) {
    const std::string &normalized_path = path;
    if (allowed_base.length() >= normalized_path.length() &&
        allowed_base.compare(allowed_base.length() - normalized_path.length(),
                             normalized_path.length(), normalized_path) == 0) {
      allowed = true;
      selected_url = allowed_base;
      break;
    }
  }

  return allowed;
}

std::string L2Worker::construct_l2_url(const std::string &selected_url,
                                       const std::string &path) {
  const std::string base_url = extract_scheme_host_port(selected_url);

  // Pre-allocate URL string to avoid multiple allocations
  std::string url;
  url.reserve(base_url.length() + path.length() +
              1); // +1 for potential leading slash

  // Ensure path starts with "/" for proper URL construction
  const std::string normalized_path = normalize_path(path);

  // normalize_path() guarantees a non-empty path starting with '/', so only
  // the base_url trailing slash decides whether a double slash would occur.
  if (!base_url.empty() && base_url.back() == '/') {
    // Remove trailing slash from base URL to avoid double slashes
    url = base_url.substr(0, base_url.length() - 1) + normalized_path;
  } else {
    url = base_url + normalized_path;
  }

  return url;
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
        403,
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
  // Use the provided l2_call_span_id if available, otherwise generate one
  const std::string actual_l2_call_span_id =
      l2_call_span_id.empty()
          ? JaegerSpanLogger::generate_span_id(m_ctx.m_tracer.get())
          : l2_call_span_id;
  // Rebuild the traceparent sent to the L2 server so the span id it links to
  // matches the span we log below. handle_trace_context generates a fresh
  // span id, which would orphan the L2 server span (parent not in trace).
  const std::string traceparent_result =
      m_ctx.m_tracer && !l2_trace_ctx.m_trace_id.empty()
          ? m_ctx.m_tracer->generate_traceparent(l2_trace_ctx.m_trace_id,
                                                 actual_l2_call_span_id,
                                                 l2_trace_ctx.m_sampled)
          : l2_trace_ctx.m_traceparent_header;

  const auto start_us = get_current_timestamp_us();

  // url is path-only: query string is appended only for the actual HTTP call
  // so that INFO logs and Jaeger http.url stay free of query parameters
  // (they may contain credentials/tokens).
  const std::string url = construct_l2_url(selected_url, path);
  Logger::debug("Calling L2 server: URL={}", url);

  int final_attempt = 0;
  HttpResponse http_response =
      execute_l2_call_with_retry(url, query, body, traceparent_result,
                                 forwarded_headers, method, final_attempt);
  const bool success =
      (http_response.m_status != 500 || !http_response.m_headers.empty());

  if (m_ctx.m_tracer && !l2_trace_ctx.m_trace_id.empty()) {
    const auto end_us = get_current_timestamp_us();
    const int span_status =
        success ? static_cast<int>(http_response.m_status) : 500;
    nlohmann::json attrs;
    if (!success) {
      // Expose how many L2 server attempts were made before giving up so the
      // retry behaviour is visible in traces (l2_call span status = 500).
      attrs["l2_call.attempts"] = final_attempt;
    }
    JaegerSpanLogger::log_l2_call(
        m_ctx.m_tracer.get(), method, url, span_status, start_us, end_us,
        l2_trace_ctx, m_ctx.m_config.m_mode, actual_l2_call_span_id,
        resolve_parent_id(parent_span_id, l2_trace_ctx.m_parent_id), "", attrs);
  }

  return http_response;
}

void L2Worker::run() {
  Logger::info("C++ L2 Worker started. Waiting for requests...");

  run_with_nats();

  Logger::info("Shutting down gracefully...");

  // Thread pool destructor drains all in-flight tasks before joining
  Logger::info("Waiting for in-flight requests to complete...");
  if (m_thread_pool) {
    m_thread_pool.reset();
  }
}

bool L2Worker::is_nats_connected() const {
  return m_nats_client && m_nats_client->is_connected();
}

bool L2Worker::validate_l2_server_access(const std::string &path,
                                         std::string &selected_url) {
  if (!is_l2_server_allowed(path, selected_url)) {
    Logger::error("Access to arbitrary L2 server address is forbidden: "
                  "attempted path = {} L2_SERVER_URLS = {}",
                  path, selected_url);
    return false;
  }
  return true;
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
        503, "L2 server circuit breaker open, service temporarily unavailable");
  }

  const int max_retries = m_ctx.m_config.m_max_retries;
  std::runtime_error last_error("Initial error");

  for (int attempt = 1; attempt <= max_retries; ++attempt) {
    final_attempt = attempt;
    try {
      const auto [http_response, http_code] = execute_http_command_with_status(
          m_http_client_pool.get(),
          [&url, &query, &body, &traceparent, &forwarded_headers,
           &method](HttpClient *client) {
            // Append the query string only to the request URL; the `url`
            // variable (used for logging) stays clean.
            std::string request_url = url;
            if (!query.empty()) {
              request_url += "?";
              request_url += query;
            }
            if (method == "POST") {
              return client->post(request_url, body, traceparent,
                                  forwarded_headers);
            } else {
              return client->get(request_url, traceparent, forwarded_headers);
            }
          });

      size_t response_size = http_response.m_body.size();

      const std::string x_real_ip =
          get_header_value(forwarded_headers, "X-Real-IP");
      const std::string x_datahub_client_id =
          get_header_value(forwarded_headers, "X-DataHub-Client-Id");
      const std::string user_agent =
          shorten_user_agent(get_header_value(forwarded_headers, "User-Agent"));

      Logger::info("L2 server call completed: method={} url={} status={} "
                   "response_size={} bytes request_size={} x_real_ip={} "
                   "x_datahub_client_id={} user_agent={}",
                   method, url, http_code, response_size, body.size(),
                   x_real_ip, x_datahub_client_id, user_agent);

      if ((http_code == 502 || http_code == 503 || http_code == 504) &&
          attempt < max_retries) {
        Logger::warn("L2 server returned {} on attempt {} url: {}. Retrying...",
                     http_code, attempt, url);
        sleep_for_attempt_jitter(attempt);
        continue;
      }

      if (http_code >= 200 && http_code < 400) {
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
        sleep_for_attempt_jitter(attempt);
      } else {
        handle_http_error(std::string(e.what()),
                          &m_ctx.m_worker.m_metrics->m_l2_errors,
                          "L2 server call", max_retries, url);
      }
    }
  }

  m_circuit_breaker.record_failure();
  return make_error_response(
      500,
      std::format("Failed to call L2 server ( url: {} ) after {} attempts: {}",
                  url, max_retries, last_error.what()));
}

void L2Worker::record_l2_call_metrics(uint64_t start_us) {
  const uint64_t end_us = get_current_timestamp_us();
  const double duration_seconds = TimeUtils::duration_seconds(start_us, end_us);
  try {
    m_ctx.m_worker.m_metrics->m_request_duration_seconds.Observe(
        duration_seconds);
  } catch (...) {
    Logger::error("Failed to record request duration histogram");
  }
}

// ============================================================================
// Circuit Breaker
// ============================================================================

void L2Worker::CircuitBreaker::set_gauge(prometheus::Gauge *gauge) {
  m_gauge = gauge;
  update_gauge();
}

void L2Worker::CircuitBreaker::update_gauge() {
  if (m_gauge != nullptr) {
    m_gauge->Set(static_cast<double>(m_state.load()));
  }
}

bool L2Worker::CircuitBreaker::allow_request() {
  const auto current_state = m_state.load();
  if (current_state == State::CLOSED) {
    return true;
  }
  if (current_state == State::HALF_OPEN) {
    return true;
  }
  // OPEN state: check if timeout has elapsed
  const uint64_t now_us = get_current_timestamp_us();
  const uint64_t elapsed = now_us - m_last_failure_time_us.load();
  if (elapsed >= g_open_timeout_us) {
    Logger::info("Circuit breaker: OPEN -> HALF_OPEN (timeout elapsed)");
    m_state.store(State::HALF_OPEN);
    m_success_count.store(0);
    update_gauge();
    return true;
  }
  return false;
}

void L2Worker::CircuitBreaker::record_success() {
  const auto current_state = m_state.load();
  if (current_state == State::HALF_OPEN) {
    const int count = m_success_count.fetch_add(1) + 1;
    if (count >= g_half_open_success_threshold) {
      Logger::info("Circuit breaker: HALF_OPEN -> CLOSED (successes={})",
                   count);
      m_state.store(State::CLOSED);
      m_failure_count.store(0);
      m_success_count.store(0);
      update_gauge();
    }
  } else if (current_state == State::CLOSED) {
    m_failure_count.store(0);
  }
}

void L2Worker::CircuitBreaker::record_failure() {
  m_last_failure_time_us.store(get_current_timestamp_us());
  const auto current_state = m_state.load();
  if (current_state == State::HALF_OPEN) {
    Logger::warn("Circuit breaker: HALF_OPEN -> OPEN (test request failed)");
    m_state.store(State::OPEN);
    m_success_count.store(0);
    update_gauge();
  } else if (current_state == State::CLOSED) {
    const int count = m_failure_count.fetch_add(1) + 1;
    if (count >= g_failure_threshold) {
      Logger::warn("Circuit breaker: CLOSED -> OPEN (failures={})", count);
      m_state.store(State::OPEN);
      update_gauge();
    }
  }
  // OPEN state: already tracking via last_failure_time_us
}

std::string L2Worker::CircuitBreaker::state_name() const {
  switch (m_state.load()) {
  case State::CLOSED:
    return "CLOSED";
  case State::OPEN:
    return "OPEN";
  case State::HALF_OPEN:
    return "HALF_OPEN";
  }
  return "UNKNOWN";
}

// ============================================================================
// Pipeline Stage Implementations for NATS request processing
// ============================================================================

bool L2Worker::parse_request_data(const std::string &request_json,
                                  json &request_data) {
  auto parse_result = validate_and_parse_json(request_json, "Worker");
  if (!parse_result) {
    return false;
  }
  request_data = std::move(*parse_result);

  // Validate request schema
  std::string parse_error;
  static RequestValidator validator = create_standard_request_validator();
  if (!validator.validate(request_data, parse_error)) {
    Logger::error("Worker request validation failed: {}", parse_error);
    handle_processing_error_with_category(
        parse_error,
        ProcessingErrorMetrics{
            .m_total_errors = nullptr,
            .m_json_errors =
                &m_ctx.m_worker.m_metrics->m_processing_json_errors,
            .m_validation_errors =
                &m_ctx.m_worker.m_metrics->m_processing_validation_errors},
        "Request Schema Validation");
    return false;
  }

  const std::string method = request_data[NatsContract::kMethod];
  if (method != "POST" && method != "GET") {
    Logger::error("Skipping non-POST request: {}", method);
    return false;
  }

  return true;
}

L2Worker::RequestData
L2Worker::extract_request_metadata(const json &request_data) {
  RequestData metadata;

  // Use explicit .get<std::string>() for clarity and potential move
  // optimization
  metadata.m_request_id =
      request_data[NatsContract::kRequestId].get<std::string>();
  metadata.m_path = request_data[NatsContract::kPath].get<std::string>();
  metadata.m_query = request_data.value(NatsContract::kQuery, std::string{});
  metadata.m_method = request_data[NatsContract::kMethod].get<std::string>();

  // Extract body as-is (compression was removed)
  metadata.m_body = request_data[NatsContract::kBody].get<std::string>();

  // Extract optional fields with defaults
  metadata.m_client_ip =
      request_data.value(NatsContract::kClientIp, std::string("unknown"));
  metadata.m_proxy_ip =
      request_data.value(NatsContract::kProxyIp, std::string("unknown"));
  metadata.m_proxy_traceparent =
      request_data.value(NatsContract::kProxyTraceparent, std::string{});
  metadata.m_proxy_span_id =
      request_data.value(NatsContract::kProxySpanId, std::string{});

  // Extract traceparent if present
  metadata.m_traceparent =
      request_data.value(NatsContract::kTraceparent, std::string{});

  const std::string effective_traceparent =
      !metadata.m_proxy_traceparent.empty() ? metadata.m_proxy_traceparent
                                            : metadata.m_traceparent;
  metadata.m_trace_ctx =
      handle_trace_context(effective_traceparent, m_ctx.m_tracer.get());

  extract_forwarded_headers(request_data, metadata.m_forwarded_headers);

  return metadata;
}

L2Worker::TracingSpans
L2Worker::create_tracing_spans(const TraceContext &parent_trace_ctx,
                               const std::string &op_parent_span_id) {
  TracingSpans spans;

  spans.m_worker_process_parent_id = op_parent_span_id;

  if (m_ctx.m_tracer && !parent_trace_ctx.m_trace_id.empty()) {
    spans.m_worker_process_span_id = m_ctx.m_tracer->generate_span_id();
    Logger::debug("Worker processing span created: {} parent: {}",
                  spans.m_worker_process_span_id,
                  spans.m_worker_process_parent_id);

    spans.m_l2_call_span_id = m_ctx.m_tracer->generate_span_id();
    spans.m_traceparent_header = m_ctx.m_tracer->generate_traceparent(
        parent_trace_ctx.m_trace_id, spans.m_l2_call_span_id, true);
    Logger::debug("Worker l2_call_span_id: {} traceparent_header: {}",
                  spans.m_l2_call_span_id, spans.m_traceparent_header);
  }

  spans.m_setex_span_id =
      JaegerSpanLogger::generate_span_id(m_ctx.m_tracer.get());

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
  data.m_is_binary = (data.m_content_type.find("image/") != std::string::npos ||
                      data.m_content_type.find("application/octet-stream") !=
                          std::string::npos ||
                      data.m_content_type.find("audio/") != std::string::npos ||
                      data.m_content_type.find("video/") != std::string::npos);

  Logger::debug("Worker checking response type: content_type={} is_binary={}",
                data.m_content_type, data.m_is_binary);

  return data;
}

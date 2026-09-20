#include "trace_logger.hpp"
#include "http_client_pool.hpp"
#include "logger.hpp"
#include "time_utils.hpp"
#include <algorithm>
#include <format>
#include <random>

// Thread-local random generator for fast ID generation (initialized once per
// thread)
static thread_local std::random_device g_rd;
static thread_local std::mt19937_64 g_gen(g_rd());
static thread_local std::uniform_int_distribution<uint64_t> g_dis;

namespace {
// Jaeger sender pool is deliberately tiny and short-timeout: spans are
// fire-and-forget, a blocked sender must never stall the request path.
constexpr size_t kJaegerPoolMaxSize = 3;
constexpr int kJaegerPoolTimeoutSeconds = 2;
constexpr int kJaegerPoolAcquireTimeoutSeconds = 3;
}

JaegerLogger::JaegerLogger(const std::string &endpoint,
                           prometheus::Counter &spans_sent,
                           prometheus::Counter &spans_failed,
                           prometheus::Gauge &queue_size,
                           prometheus::Gauge &last_send_duration,
                           prometheus::Histogram &send_latency,
                           prometheus::Histogram &queue_time, size_t batch_size,
                           int flush_interval_ms, double sample_rate,
                           const std::string &sentry_dsn,
                           const std::string &sentry_service,
                           const std::string &sentry_environment,
                           const std::string &sentry_release,
                           prometheus::Counter *sentry_spans_sent,
                           prometheus::Counter *sentry_spans_failed,
                           double sentry_sample_rate,
                           TracingBreakerSettings breaker_settings)
    : m_jaeger({endpoint, batch_size, flush_interval_ms, sample_rate}),
      m_jaeger_metrics({spans_sent, spans_failed, queue_size,
                        last_send_duration, send_latency, queue_time}),
      m_sentry_metrics({sentry_spans_sent, sentry_spans_failed}),
      m_pools{.m_http_client_pool = std::make_unique<HttpClientPool>(
                  kJaegerPoolMaxSize, kJaegerPoolTimeoutSeconds,
                  kJaegerPoolAcquireTimeoutSeconds, true)},
      m_sentry({sentry::parse_dsn(sentry_dsn), sentry_service,
                sentry_environment, sentry_release, sentry_sample_rate}),
      m_breaker({}, {}, breaker_settings) {
  Logger::info("JaegerLogger initialized: batch_size={} flush_interval={}ms "
               "sample_rate={}",
               m_jaeger.m_batch_size, m_jaeger.m_flush_interval_ms,
               m_jaeger.m_sample_rate);
  if (m_sentry.m_dsn) {
    m_pools.m_sentry_client_pool = std::make_unique<HttpClientPool>(
        kJaegerPoolMaxSize, kJaegerPoolTimeoutSeconds,
        kJaegerPoolAcquireTimeoutSeconds, true);
    Logger::info("JaegerLogger Sentry performance target enabled: host={} "
                 "project={} service={}",
                 m_sentry.m_dsn->m_host, m_sentry.m_dsn->m_project_id,
                 m_sentry.m_service);
  }
  m_delivery.m_sender_thread =
      std::jthread([this](const std::stop_token &st) { sender_loop(st); });
}

JaegerLogger::~JaegerLogger() {
  if (m_delivery.m_sender_thread.joinable()) {
    m_delivery.m_sender_thread.request_stop();
    m_delivery.m_span_queue.m_cv.notify_all();
    m_delivery.m_sender_thread.join();
  }
}

std::string JaegerLogger::generate_trace_id() { return random_hex_fast(32); }

std::string JaegerLogger::generate_span_id() { return random_hex_fast(16); }

// W3C Trace Context helpers
std::string JaegerLogger::generate_traceparent(std::string_view trace_id,
                                               std::string_view span_id,
                                               bool sampled) {
  return std::format("00-{}-{}-{}", trace_id, span_id, sampled ? "01" : "00");
}

// Simplified validation - check key positions only
bool JaegerLogger::validate_traceparent(std::string_view traceparent) {
  // Format: 00-{32hex}-{16hex}-{2hex} = 55 chars
  if (traceparent.size() != 55)
    return false;

  // Check version prefix
  if (traceparent[0] != '0' || traceparent[1] != '0' || traceparent[2] != '-')
    return false;

  // Check separators at key positions
  if (traceparent[35] != '-' || traceparent[52] != '-')
    return false;

  // Quick hex validation at sample positions (not all for performance)
  const auto is_hex = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  };

  // Check trace_id start and end
  if (!is_hex(traceparent[3]) || !is_hex(traceparent[34]))
    return false;
  // Check span_id start and end
  if (!is_hex(traceparent[36]) || !is_hex(traceparent[51]))
    return false;
  // Check flags
  if (!is_hex(traceparent[53]) || !is_hex(traceparent[54]))
    return false;

  // Validate flags (must be 00 or 01)
  if (traceparent[53] != '0')
    return false;
  if (traceparent[54] != '0' && traceparent[54] != '1')
    return false;

  return true;
}

bool JaegerLogger::parse_traceparent(std::string_view traceparent,
                                     std::string &trace_id,
                                     std::string &parent_span_id,
                                     bool &sampled) {
  if (!validate_traceparent(traceparent)) {
    // warn (not error): a broken client header is user input from a hot path
    // (l2_worker/request_handler) and must not flood the ERROR log.
    Logger::warn("Invalid traceparent header: {}", traceparent);
    return false;
  }

  // Extract trace-id (32 chars after "00-")
  trace_id = std::string(traceparent.substr(3, 32));
  // Extract parent-id (16 chars after trace-id and "-")
  parent_span_id = std::string(traceparent.substr(36, 16));
  // Extract flags (2 chars at end)
  auto flags = traceparent.substr(53, 2);

  sampled = (flags == "01");

  return true;
}

bool JaegerLogger::should_sample() const {
  if (m_jaeger.m_sample_rate >= 1.0)
    return true;
  if (m_jaeger.m_sample_rate <= 0.0)
    return false;

  // Thread-local generator for sampling decision
  thread_local std::uniform_real_distribution<> dis(0.0, 1.0);
  return dis(g_gen) < m_jaeger.m_sample_rate;
}

bool JaegerLogger::should_sample(bool is_error) const {
  // Always sample error spans for debugging
  if (is_error)
    return true;

  // Apply normal sampling for non-error spans
  return should_sample();
}

std::string JaegerLogger::sentry_span_op(const std::string &name) {
  if (name.starts_with("HTTP ")) {
    const size_t sp = name.find(' ', 5);
    const std::string token =
        sp == std::string::npos ? name.substr(5) : name.substr(5, sp - 5);
    if (token.starts_with("NATS")) {
      return "messaging";
    }
    if (token.starts_with("DB")) {
      return "db";
    }
    if (token.starts_with("L2")) {
      return "http.client";
    }
    return "http.server";
  }
  if (name.starts_with("NATS")) {
    return "messaging";
  }
  if (name.starts_with("DB")) {
    return "db";
  }
  return "span";
}

std::string JaegerLogger::sentry_transaction_status(int status_code) {
  if (status_code >= 500) {
    return "internal_error";
  }
  return "ok";
}

nlohmann::json JaegerLogger::build_sentry_transaction_json(
    const std::string &trace_id, const std::string &span_id,
    const std::string &parent_id, const std::string &name,
    const std::string &service_name, uint64_t start_us, uint64_t end_us,
    const std::string &environment, const nlohmann::json &attributes,
    const std::string &release, const std::string &product) {
  nlohmann::json root = nlohmann::json::object();
  root["event_id"] = random_hex_fast(32);
  root["type"] = "transaction";
  root["start_timestamp"] = TimeUtils::format_rfc3339_us(start_us);
  root["timestamp"] = TimeUtils::format_rfc3339_us(end_us);
  root["platform"] = "native";
  root["transaction"] =
      product.empty() ? name : product + ": " + name;
  if (!release.empty()) {
    root["release"] = release;
  }

  int status_code = 200;
  if (attributes.is_object() && attributes.contains("http.status_code")) {
    const auto &code = attributes["http.status_code"];
    if (code.is_number_integer() || code.is_number_unsigned()) {
      status_code = code.get<int>();
    }
  }
  nlohmann::json trace_ctx = {
      {"trace_id", trace_id},
      {"span_id", span_id},
      {"op", sentry_span_op(name)},
      {"status", sentry_transaction_status(status_code)},
  };
  if (!parent_id.empty()) {
    trace_ctx["parent_span_id"] = parent_id;
  }
  nlohmann::json contexts = nlohmann::json::object();
  contexts["trace"] = std::move(trace_ctx);
  if (!service_name.empty()) {
    contexts["service"] = nlohmann::json{{"name", service_name}};
  }
  root["contexts"] = std::move(contexts);
  if (!environment.empty()) {
    root["environment"] = environment;
  }

  nlohmann::json tags = nlohmann::json::object();
  if (!service_name.empty()) {
    tags["service"] = service_name;
  }
  if (!product.empty()) {
    tags["mode"] = product;
  }
  if (attributes.is_object()) {
    for (const auto &entry : attributes.items()) {
      if (entry.value().is_string()) {
        tags[entry.key()] = entry.value();
      } else {
        tags[entry.key()] = entry.value().dump();
      }
    }
  }
  root["tags"] = std::move(tags);

  if (attributes.is_object() && !attributes.empty()) {
    root["extra"] = attributes;
  }
  return root;
}

std::string JaegerLogger::sentry_envelope_url(const sentry::DsnData &dsn) {
  const std::string scheme = dsn.m_scheme.empty() ? "http" : dsn.m_scheme;
  return scheme + "://" + dsn.m_host + ":" + std::to_string(dsn.m_port) +
         dsn.m_path_prefix + "/api/" + dsn.m_project_id + "/envelope/";
}

std::string JaegerLogger::sentry_auth_header(const sentry::DsnData &dsn) {
  std::string key = dsn.m_public_key;
  if (!dsn.m_secret_key.empty()) {
    key += "/" + dsn.m_secret_key;
  }
  return "Sentry sentry_version=7, sentry_key=" + key;
}

std::string JaegerLogger::build_sentry_transaction_envelope(
    const std::string &trace_id, const std::string &span_id,
    const std::string &parent_id, const std::string &name,
    const std::string &service_name, uint64_t start_us, uint64_t end_us,
    const sentry::DsnData &dsn, const std::string &environment,
    const nlohmann::json &attributes, const std::string &release,
    const std::string &product) {
  const nlohmann::json event_json = build_sentry_transaction_json(
      trace_id, span_id, parent_id, name, service_name, start_us, end_us,
      environment, attributes, release, product);
  return build_sentry_transaction_envelope(event_json);
}

std::string JaegerLogger::build_sentry_transaction_envelope(
    const nlohmann::json &event_json) {
  const std::string event_id = event_json.value("event_id", "");
  const nlohmann::json header = {
      {"event_id", event_id},
      {"sent_at", TimeUtils::format_rfc3339()},
      {"sdk", {{"name", "http-data-diod"}, {"version", "0"}}}};
  const nlohmann::json item_header = {{"type", "transaction"}};
  return header.dump() + "\n" + item_header.dump() + "\n" + event_json.dump();
}

std::string JaegerLogger::build_sentry_envelope(
    const std::vector<nlohmann::json> &events) {
  if (events.empty()) {
    return "";
  }
  std::string header_event_id = events.front().value("event_id", "");
  const nlohmann::json header = {
      {"event_id", header_event_id},
      {"sent_at", TimeUtils::format_rfc3339()},
      {"sdk", {{"name", "http-data-diod"}, {"version", "0"}}}};
  std::string envelope = header.dump();
  for (const auto &event : events) {
    const std::string payload = event.dump();
    const nlohmann::json item_header = {
        {"type", "transaction"}, {"length", payload.size()}};
    envelope += "\n" + item_header.dump() + "\n" + payload;
  }
  return envelope;
}

void JaegerLogger::deliver_sentry_transactions(
    const std::vector<SpanData> &batch) {
  if (!m_sentry.m_dsn || !m_pools.m_sentry_client_pool || batch.empty()) {
    return;
  }
  // Circuit breaker (Sentry/GlitchTip has no built-in retry loop, so a long
  // outage would otherwise fire one multi-item envelope POST per batch
  // forever). After the consecutive-failure threshold the breaker opens:
  // delivery is skipped for the exponential cooldown window (capped) and the
  // batch is shed instead of bombarding the dead target.
  const uint64_t now_steady_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  const uint64_t cooldown_until =
      m_breaker.m_sentry.cooldown_until_steady_ms.load();
  if (cooldown_until != 0 && now_steady_ms < cooldown_until) {
    m_jaeger_metrics.m_spans_failed.Increment(batch.size());
    if (m_sentry_metrics.m_spans_failed) {
      m_sentry_metrics.m_spans_failed->Increment(batch.size());
    }
    return;
  }
  // Sentry target has its own sample rate; per-span Bernoulli draw keeps the
  // loop deterministic (rate 1.0 → always send, rate 0.0 → never send).
  thread_local std::uniform_real_distribution<> dis(0.0, 1.0);
  std::vector<nlohmann::json> events;
  events.reserve(batch.size());
  for (const auto &span : batch) {
    if (m_sentry.m_sample_rate < 1.0 &&
        dis(g_gen) >= m_sentry.m_sample_rate) {
      continue;
    }
    events.push_back(build_sentry_transaction_json(
        span.m_trace_id, span.m_span_id, span.m_parent_id, span.m_name,
        span.m_service_name, span.m_start_us, span.m_end_us,
        m_sentry.m_environment, span.m_attributes, m_sentry.m_release,
        m_sentry.m_service));
  }
  if (events.empty()) {
    return;
  }
  const size_t event_count = events.size();
  const std::string url = sentry_envelope_url(*m_sentry.m_dsn);
  const std::string auth = sentry_auth_header(*m_sentry.m_dsn);
  // One multi-item envelope per batch: a single POST carries every sampled
  // transaction of the batch.
  const std::string envelope = build_sentry_envelope(events);
  try {
    auto client = m_pools.m_sentry_client_pool->acquire_connection();
    if (!client) {
      if (m_sentry_metrics.m_spans_failed) {
        m_sentry_metrics.m_spans_failed->Increment(event_count);
      }
      return;
    }
    client->post_no_response(
        url, envelope, "",
        httplib::Headers{{"X-Sentry-Auth", auth}},
        "application/x-sentry-envelope");
    m_pools.m_sentry_client_pool->release_connection(std::move(client));
    m_breaker.m_sentry.consecutive_failures = 0;
    m_breaker.m_sentry.cooldown_until_steady_ms = 0;
    if (m_sentry_metrics.m_spans_sent) {
      m_sentry_metrics.m_spans_sent->Increment(event_count);
    }
  } catch (const std::exception &e) {
    Logger::error(
        "Sentry transaction delivery failed (non-critical): {}", e.what());
    m_breaker.m_sentry.consecutive_failures++;
    if (m_breaker.m_sentry.consecutive_failures >=
        m_breaker.m_settings.m_failure_threshold) {
      uint64_t cooldown_ms =
          std::min<uint64_t>(
              (uint64_t)m_breaker.m_settings.m_cooldown_base_ms
                  << (std::min(m_breaker.m_sentry.consecutive_failures.load(),
                               20) -
                      1),
              (uint64_t)m_breaker.m_settings.m_cooldown_max_ms);
      m_breaker.m_sentry.cooldown_until_steady_ms =
          now_steady_ms + cooldown_ms;
      Logger::warn("Sentry breaker opened, shedding for {}ms", cooldown_ms);
    }
    if (m_sentry_metrics.m_spans_failed) {
      m_sentry_metrics.m_spans_failed->Increment(event_count);
    }
  } catch (...) {
    Logger::error(
        "Sentry transaction delivery failed with unknown error "
        "(non-critical)");
    m_breaker.m_sentry.consecutive_failures++;
    if (m_breaker.m_sentry.consecutive_failures >=
        m_breaker.m_settings.m_failure_threshold) {
      uint64_t cooldown_ms =
          std::min<uint64_t>(
              (uint64_t)m_breaker.m_settings.m_cooldown_base_ms
                  << (std::min(m_breaker.m_sentry.consecutive_failures.load(),
                               20) -
                      1),
              (uint64_t)m_breaker.m_settings.m_cooldown_max_ms);
      m_breaker.m_sentry.cooldown_until_steady_ms =
          now_steady_ms + cooldown_ms;
      Logger::warn("Sentry breaker opened, shedding for {}ms", cooldown_ms);
    }
    if (m_sentry_metrics.m_spans_failed) {
      m_sentry_metrics.m_spans_failed->Increment(event_count);
    }
  }
}

void JaegerLogger::enqueue_span(const std::string &trace_id,
                                 const std::string &span_id,
                                 const std::string &parent_id,
                                 const std::string &name, uint64_t start_us,
                                 uint64_t end_us,
                                 const std::string &service_name,
                                 const nlohmann::json &attributes) {
  {
    std::unique_lock lock(m_delivery.m_span_queue.m_mutex);
    if (m_delivery.m_span_queue.m_spans.size() >= g_tracing_max_queue_size) {
      m_jaeger_metrics.m_spans_failed.Increment();
      Logger::warn("Tracing queue full, dropped span: trace_id={}", trace_id);
      return;
    }
    uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
    m_delivery.m_span_queue.m_spans.push_back({trace_id, span_id, parent_id, name,
                                    service_name, start_us, end_us, now_us,
                                    attributes});
    m_jaeger_metrics.m_queue_size.Set(m_delivery.m_span_queue.m_spans.size());
  }
  m_delivery.m_span_queue.m_cv.notify_one();
}

void JaegerLogger::log_request(
    const std::string &method, const std::string &url, int status_code,
    uint64_t start_us, uint64_t end_us, const std::string &service_name,
    const std::string &request_id, const std::string &trace_id,
    const std::string &span_id, const std::string &parent_id,
    const nlohmann::json &additional_attributes,
    const std::string &name_override) {
  // Apply sampling - always sample errors (4xx and 5xx status codes)
  bool is_error = (status_code >= 400);
  if (!should_sample(is_error)) {
    return;
  }

  auto actual_trace_id =
      trace_id.empty() ? generate_trace_id() : trace_id;
  auto actual_span_id = span_id.empty() ? generate_span_id() : span_id;

  // Non-HTTP operations (NATS, messaging) override the display name; the
  // default "HTTP {method} {url}" convention stays for actual HTTP calls.
  const std::string span_name = name_override.empty()
                                    ? std::format("HTTP {} {}", method, url)
                                    : name_override;

  Logger::debug(
      "Logging span: trace_id={} span_id={} operation={} service={}",
      actual_trace_id, actual_span_id, span_name, service_name);

  nlohmann::json attrs = nlohmann::json::object();
  attrs["http.method"] = method;
  attrs["http.url"] = url;
  attrs["http.status_code"] = status_code;

  if (!request_id.empty()) {
    attrs["request.id"] = request_id;
  }

  for (const auto &el : additional_attributes.items()) {
    attrs[el.key()] = el.value();
  }

  enqueue_span(actual_trace_id, actual_span_id, parent_id, span_name, start_us,
               end_us, service_name, attrs);
}

// Fast random hex generation using pre-initialized thread-local generator
std::string JaegerLogger::random_hex_fast(size_t len) {
  std::string res;
  res.reserve(len);

  static const char g_hex_chars[] = "0123456789abcdef";

  // Generate 8 chars at a time (64 bits)
  for (size_t i = 0; i < len; i += 16) {
    uint64_t val = g_dis(g_gen);
    for (size_t j = 0; j < 16 && (i + j) < len; ++j) {
      res += g_hex_chars[(val >> (j * 4)) & 0xF];
    }
  }

  return res;
}

void JaegerLogger::sender_loop(std::stop_token st) {
  while (!st.stop_requested()) {
    std::vector<SpanData> batch;
    batch.reserve(m_jaeger.m_batch_size);

    {
      std::unique_lock lock(m_delivery.m_span_queue.m_mutex);
      // Wait for work or stop — replaces poll sleep 100ms
      if (m_delivery.m_span_queue.m_spans.empty()) {
        m_delivery.m_span_queue.m_cv.wait_for(
            lock, st,
            std::chrono::milliseconds(m_jaeger.m_flush_interval_ms / 10),
            [&] {
              return st.stop_requested() || !m_delivery.m_span_queue.m_spans.empty();
            });
        if (st.stop_requested()) break;
        if (m_delivery.m_span_queue.m_spans.empty()) continue;
      }
      while (!m_delivery.m_span_queue.m_spans.empty() &&
             batch.size() < m_jaeger.m_batch_size) {
        batch.push_back(std::move(m_delivery.m_span_queue.m_spans.front()));
        m_delivery.m_span_queue.m_spans.pop_front();
      }
      m_jaeger_metrics.m_queue_size.Set(m_delivery.m_span_queue.m_spans.size());
    }

    if (!batch.empty()) {
      send_batch_with_retry(batch, st);
    }
  }

  // Flush remaining spans on shutdown (quick, non-blocking)
  std::vector<SpanData> final_batch;
  {
    std::unique_lock lock(m_delivery.m_span_queue.m_mutex);
    while (!m_delivery.m_span_queue.m_spans.empty()) {
      final_batch.push_back(std::move(m_delivery.m_span_queue.m_spans.front()));
      m_delivery.m_span_queue.m_spans.pop_front();
    }
    m_jaeger_metrics.m_queue_size.Set(0);
  }

  if (!final_batch.empty()) {
    Logger::debug("Flushing {} remaining spans on shutdown",
                  final_batch.size());

    // Quick fire-and-forget send (no retries on shutdown)
    if (send_batch(final_batch)) {
      m_jaeger_metrics.m_spans_sent.Increment(final_batch.size());
      Logger::debug("Successfully flushed {} spans", final_batch.size());
    } else {
      m_jaeger_metrics.m_spans_failed.Increment(final_batch.size());
      Logger::debug("Dropped {} spans on shutdown (non-critical)",
                    final_batch.size());
    }
    deliver_sentry_transactions(final_batch);
  }
}

void JaegerLogger::send_batch_with_retry(const std::vector<SpanData> &batch,
                                         const std::stop_token &st) {
  const auto start_time = std::chrono::steady_clock::now();
  uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
  double avg_queue_time_s = 0.0;
  for (const auto &span : batch) {
    avg_queue_time_s +=
        TimeUtils::duration_seconds(span.m_enqueue_time_us, now_us);
  }
  avg_queue_time_s /= batch.size();
  m_jaeger_metrics.m_queue_time.Observe(avg_queue_time_s);

  bool success = false;
  int retries = 0;
  int delay_ms = g_tracing_retry_base_delay_ms;

  // Circuit breaker (Jaeger). After the consecutive-failure threshold the
  // breaker opens, delivery of the batch is shed for the exponential cooldown
  // window (capped) instead of bombarding the dead target with a fresh batch
  // of retries every flush interval forever. Closing the breaker resets the
  // consecutive-failure counter.
  const auto now_ms = TimeUtils::steady_ms();
  if (now_ms < m_breaker.m_jaeger.cooldown_until_steady_ms.load()) {
    m_jaeger_metrics.m_spans_failed.Increment(batch.size());
    Logger::debug(
        "Jaeger breaker open, shed batch of {} spans during cooldown", 
        batch.size());
    deliver_sentry_transactions(batch);
    const auto end_time = std::chrono::steady_clock::now();
    double duration =
        std::chrono::duration<double>(end_time - start_time).count();
    m_jaeger_metrics.m_last_send_duration.Set(duration);
    m_jaeger_metrics.m_send_latency.Observe(duration);
    return;
  }

  while (!success && retries < g_tracing_max_retries && !st.stop_requested()) {
    if (send_batch(batch)) {
      success = true;
      m_breaker.m_jaeger.consecutive_failures = 0;
      m_breaker.m_jaeger.cooldown_until_steady_ms = 0;
    } else {
      retries++;
      m_breaker.m_jaeger.consecutive_failures++;
      Logger::warn("Jaeger batch send failed, retry {}/{} in {}ms", retries,
                   g_tracing_max_retries, delay_ms);
      if (m_breaker.m_jaeger.consecutive_failures >=
          m_breaker.m_settings.m_failure_threshold) {
        uint64_t cooldown_ms =
            std::min<uint64_t>(
                (uint64_t)m_breaker.m_settings.m_cooldown_base_ms
                    << (std::min(m_breaker.m_jaeger.consecutive_failures.load(),
                                 20) -
                        1),
                (uint64_t)m_breaker.m_settings.m_cooldown_max_ms);
        m_breaker.m_jaeger.cooldown_until_steady_ms =
            now_ms + cooldown_ms;
        Logger::warn("Jaeger breaker opened, shedding for {}ms", cooldown_ms);
      }
      if (retries < g_tracing_max_retries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        delay_ms *= 2;
      }
    }
  }

  if (success) {
    m_jaeger_metrics.m_spans_sent.Increment(batch.size());
  } else {
    m_jaeger_metrics.m_spans_failed.Increment(batch.size());
    Logger::error(
        "Jaeger batch send failed after {} retries, dropped {} spans",
        retries, batch.size());
  }

  // Sentry/GlitchTip performance delivery is once-per-batch (independent of the
  // Jaeger retry loop so a temporary Jaeger outage cannot duplicate
  // transactions) and fire-and-forget like the Jaeger send itself.
  deliver_sentry_transactions(batch);

  const auto end_time = std::chrono::steady_clock::now();
  double duration =
      std::chrono::duration<double>(end_time - start_time).count();
  m_jaeger_metrics.m_last_send_duration.Set(duration);
  m_jaeger_metrics.m_send_latency.Observe(duration);
}

bool JaegerLogger::send_batch(const std::vector<SpanData> &batch) {
  if (batch.empty())
    return true;

  nlohmann::json batch_json = nlohmann::json::array();

  for (const auto &span_data : batch) {
    const auto span_json = build_span_json(
        span_data.m_trace_id, span_data.m_span_id, span_data.m_parent_id,
        span_data.m_name, span_data.m_start_us, span_data.m_end_us,
        span_data.m_service_name, span_data.m_attributes);
    batch_json.push_back(span_json);
  }

  auto payload_str = batch_json.dump();

  // Fire-and-forget: never block, never throw, never fail main logic.
  // Counter is incremented only by the caller (sender_loop / flush) to avoid
  // double-counting: send_batch returns false and the caller decides.
  try {
    // Use very short timeout for tracing (don't block on Jaeger issues)
    auto client = m_pools.m_http_client_pool->acquire_connection();
    if (!client) {
      // Pool exhausted - drop spans silently (tracing is not critical)
      return false;
    }

    // Send with very short timeout - tracing should never block
    client->post_no_response(m_jaeger.m_url, payload_str, "");
    m_pools.m_http_client_pool->release_connection(std::move(client));

    if (m_breaker.m_jaeger.consecutive_failures.load() > 0) {
      Logger::debug("Jaeger send recovered after {} failures",
                    m_breaker.m_jaeger.consecutive_failures.load());
    }
    return true;
  } catch (const std::exception &e) {
    // Tracing failures are not critical but an exception message must reach
    // the error log to debug outages.
    Logger::error("Jaeger batch send failed (non-critical): {} spans dropped: "
                  "{}",
                  batch.size(), e.what());
    return false;
  } catch (...) {
    // Catch all exceptions - tracing must never crash
    Logger::error(
        "Jaeger batch send failed with unknown error (non-critical)");
    return false;
  }
}

bool JaegerLogger::send_span(const std::string &trace_id,
                             const std::string &span_id,
                             const std::string &parent_id,
                             const std::string &name, uint64_t start_us,
                             uint64_t end_us, const std::string &service_name,
                             const nlohmann::json &attributes) {
  const auto span_json =
      build_span_json(trace_id, span_id, parent_id, name, start_us, end_us,
                      service_name, attributes);
  auto payload_str = span_json.dump();

  // Fire-and-forget: never block main logic
  try {
    auto client = m_pools.m_http_client_pool->acquire_connection();
    if (!client) {
      m_jaeger_metrics.m_spans_failed.Increment();
      return false;
    }

    client->post_no_response(m_jaeger.m_url, payload_str, "");
    m_pools.m_http_client_pool->release_connection(std::move(client));
    return true;
  } catch (const std::exception &e) {
    Logger::error("Jaeger span send failed (non-critical): {}", e.what());
    m_jaeger_metrics.m_spans_failed.Increment();
    return false;
  } catch (...) {
    Logger::error("Jaeger span send failed with unknown error (non-critical)");
    m_jaeger_metrics.m_spans_failed.Increment();
    return false;
  }
}

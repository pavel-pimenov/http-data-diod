#include "trace_logger.hpp"
#include "http_client_pool.hpp"
#include "logger.hpp"
#include <format>

// Thread-local random generator for fast ID generation (initialized once per
// thread)
static thread_local std::random_device g_rd;
static thread_local std::mt19937_64 g_gen(g_rd());
static thread_local std::uniform_int_distribution<uint64_t> g_dis;

JaegerLogger::JaegerLogger(const std::string &endpoint,
                           prometheus::Counter &spans_sent,
                           prometheus::Counter &spans_failed,
                           prometheus::Gauge &queue_size,
                           prometheus::Gauge &last_send_duration,
                           prometheus::Histogram &send_latency,
                           prometheus::Histogram &queue_time, size_t batch_size,
                           int flush_interval_ms, double sample_rate)
    : m_jaeger_url(endpoint), m_tracing_spans_sent_counter(spans_sent),
      m_tracing_spans_failed_counter(spans_failed),
      m_tracing_queue_size_gauge(queue_size),
      m_tracing_last_send_duration_gauge(last_send_duration),
      m_tracing_send_latency_histogram(send_latency),
      m_tracing_queue_time_histogram(queue_time),
      m_http_client_pool(std::make_unique<HttpClientPool>(
          3,   // Small pool for Jaeger (fire-and-forget)
          2,   // Very short timeout (2 seconds)
          3,   // Short acquire timeout (3 seconds)
          true // enable connection reuse
          )),
      m_batch_size(batch_size), m_flush_interval_ms(flush_interval_ms),
      m_sample_rate(sample_rate) {
  Logger::info("JaegerLogger initialized: batch_size={} flush_interval={}ms "
               "sample_rate={}",
               m_batch_size, m_flush_interval_ms, m_sample_rate);
  m_sender_thread = std::thread(&JaegerLogger::sender_loop, this);
}

JaegerLogger::~JaegerLogger() {
  m_stop_sender = true;
  if (m_sender_thread.joinable()) {
    m_sender_thread.join();
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
    Logger::error("Invalid traceparent header: {}", traceparent);
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

// Extract trace_id and parent_span_id from traceparent header
// Delegate to JaegerLogger::parse_traceparent so the "00-{32}-{16}-{2}"
// format (positions, lengths, hex/flags validation) lives in one place.
TraceInfo extract_trace_info(std::string_view traceparent) {
  TraceInfo info;
  if (JaegerLogger::parse_traceparent(traceparent, info.m_trace_id,
                                      info.m_parent_span_id, info.m_sampled)) {
    info.m_valid = true;
  }
  return info;
}

bool JaegerLogger::should_sample() const {
  if (m_sample_rate >= 1.0)
    return true;
  if (m_sample_rate <= 0.0)
    return false;

  // Thread-local generator for sampling decision
  thread_local std::uniform_real_distribution<> dis(0.0, 1.0);
  return dis(g_gen) < m_sample_rate;
}

bool JaegerLogger::should_sample(bool is_error) const {
  // Always sample error spans for debugging
  if (is_error)
    return true;

  // Apply normal sampling for non-error spans
  return should_sample();
}

void JaegerLogger::enqueue_span(const std::string &trace_id,
                                const std::string &span_id,
                                const std::string &parent_id,
                                const std::string &name, uint64_t start_us,
                                uint64_t end_us,
                                const std::string &service_name,
                                const nlohmann::json &attributes) {
  std::unique_lock lock(m_queue_mutex);

  // Check queue size limit - drop if full
  if (m_span_queue.size() >= g_tracing_max_queue_size) {
    m_tracing_spans_failed_counter.Increment();
    Logger::warn("Tracing queue full, dropped span: trace_id={}", trace_id);
    return;
  }

  // Calculate queue time for spans that were waiting
  uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();

  m_span_queue.push_back({trace_id, span_id, parent_id, name, service_name,
                          start_us, end_us, now_us, attributes});
  m_tracing_queue_size_gauge.Set(m_span_queue.size());
}

void JaegerLogger::log_request(
    const std::string &method, const std::string &url, int status_code,
    uint64_t start_us, uint64_t end_us, const std::string &service_name,
    const std::string &request_id, const std::string &trace_id,
    const std::string &span_id, const std::string &parent_id,
    const nlohmann::json &additional_attributes) {
  // Apply sampling - always sample errors (4xx and 5xx status codes)
  bool is_error = (status_code >= 400);
  if (!should_sample(is_error)) {
    return;
  }

  auto actual_trace_id =
      trace_id.empty() ? generate_trace_id() : trace_id;
  auto actual_span_id = span_id.empty() ? generate_span_id() : span_id;

  Logger::debug(
      "Logging span: trace_id={} span_id={} operation=HTTP {} {} service={}",
      actual_trace_id, actual_span_id, method, url, service_name);

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

  enqueue_span(actual_trace_id, actual_span_id, parent_id,
               "HTTP " + method + " " + url, start_us, end_us, service_name,
               attrs);
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

void JaegerLogger::sender_loop() {
  while (!m_stop_sender) {
    std::vector<SpanData> batch;
    batch.reserve(m_batch_size);

    {
      std::unique_lock lock(m_queue_mutex);
      while (!m_span_queue.empty() && batch.size() < m_batch_size) {
        batch.push_back(std::move(m_span_queue.front()));
        m_span_queue.pop_front();
      }
      m_tracing_queue_size_gauge.Set(m_span_queue.size());
    }

    if (!batch.empty()) {
      const auto start_time = std::chrono::steady_clock::now();

      // Calculate queue time for metrics
      uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
      double avg_queue_time_s = 0.0;
      for (const auto &span : batch) {
        avg_queue_time_s += (now_us - span.m_enqueue_time_us) / 1000000.0;
      }
      avg_queue_time_s /= batch.size();
      m_tracing_queue_time_histogram.Observe(avg_queue_time_s);

      // Send with retry logic
      bool success = false;
      int retries = 0;
      int delay_ms = g_tracing_retry_base_delay_ms;

      while (!success && retries < g_tracing_max_retries && !m_stop_sender) {
        if (send_batch(batch)) {
          success = true;
          m_consecutive_failures = 0; // Reset failure counter
        } else {
          retries++;
          m_consecutive_failures++;
          Logger::warn("Jaeger batch send failed, retry {}/{} in {}ms", retries,
                       g_tracing_max_retries, delay_ms);

          if (retries < g_tracing_max_retries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms *= 2; // Exponential backoff
          }
        }
      }

      if (success) {
        m_tracing_spans_sent_counter.Increment(batch.size());
      } else {
        m_tracing_spans_failed_counter.Increment(batch.size());
        Logger::error(
            "Jaeger batch send failed after {} retries, dropped {} spans",
            retries, batch.size());
      }

      const auto end_time = std::chrono::steady_clock::now();
      double duration =
          std::chrono::duration<double>(end_time - start_time).count();
      m_tracing_last_send_duration_gauge.Set(duration);
      m_tracing_send_latency_histogram.Observe(duration);
    } else {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(m_flush_interval_ms / 10));
    }
  }

  // Flush remaining spans on shutdown (quick, non-blocking)
  std::vector<SpanData> final_batch;
  {
    std::unique_lock lock(m_queue_mutex);
    while (!m_span_queue.empty()) {
      final_batch.push_back(std::move(m_span_queue.front()));
      m_span_queue.pop_front();
    }
    m_tracing_queue_size_gauge.Set(0);
  }

  if (!final_batch.empty()) {
    Logger::debug("Flushing {} remaining spans on shutdown",
                  final_batch.size());

    // Quick fire-and-forget send (no retries on shutdown)
    if (send_batch(final_batch)) {
      m_tracing_spans_sent_counter.Increment(final_batch.size());
      Logger::debug("Successfully flushed {} spans", final_batch.size());
    } else {
      m_tracing_spans_failed_counter.Increment(final_batch.size());
      Logger::debug("Dropped {} spans on shutdown (non-critical)",
                    final_batch.size());
    }
  }
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
    auto client = m_http_client_pool->acquire_connection();
    if (!client) {
      // Pool exhausted - drop spans silently (tracing is not critical)
      return false;
    }

    // Send with very short timeout - tracing should never block
    client->post_no_response(m_jaeger_url, payload_str, "");
    m_http_client_pool->release_connection(std::move(client));

    if (m_consecutive_failures > 0) {
      Logger::debug("Jaeger send recovered after {} failures",
                    m_consecutive_failures.load());
    }
    return true;
  } catch (const std::runtime_error &e) {
    // Log at debug level - tracing failures are not critical
    Logger::debug("Jaeger batch send failed (non-critical): {} spans dropped",
                  batch.size());
    return false;
  } catch (...) {
    // Catch all exceptions - tracing must never crash
    Logger::debug("Jaeger batch send failed with unknown error (non-critical)");
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
    auto client = m_http_client_pool->acquire_connection();
    if (!client) {
      m_tracing_spans_failed_counter.Increment();
      return false;
    }

    client->post_no_response(m_jaeger_url, payload_str, "");
    m_http_client_pool->release_connection(std::move(client));
    return true;
  } catch (const std::runtime_error &e) {
    Logger::debug("Jaeger span send failed (non-critical): {}", e.what());
    m_tracing_spans_failed_counter.Increment();
    return false;
  } catch (...) {
    Logger::debug("Jaeger span send failed with unknown error (non-critical)");
    m_tracing_spans_failed_counter.Increment();
    return false;
  }
}

// ============================================================================
// Baggage Propagation Implementation
// ============================================================================

// Thread-local baggage storage with TTL
static thread_local std::unordered_map<std::string,
                                       std::pair<Baggage, uint64_t>>
    g_trace_baggage;
static constexpr uint64_t g_baggage_ttl_us = 60000000; // 60 seconds

static void cleanup_expired_baggage() {
  uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
  for (auto it = g_trace_baggage.begin(); it != g_trace_baggage.end();) {
    if (now_us - it->second.second > g_baggage_ttl_us) {
      it = g_trace_baggage.erase(it);
    } else {
      ++it;
    }
  }
}

void JaegerLogger::set_baggage(const std::string &trace_id,
                               const std::string &key,
                               const std::string &value) {
  cleanup_expired_baggage();
  auto &entry = g_trace_baggage[trace_id];
  entry.first.set(key, value);
  entry.second = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();
  Logger::debug("Baggage set: trace_id={} key={} value={}", trace_id, key,
                value);
}

std::string JaegerLogger::get_baggage(const std::string &trace_id,
                                      const std::string &key) {
  cleanup_expired_baggage();
  const auto it = g_trace_baggage.find(trace_id);
  if (it != g_trace_baggage.end()) {
    return it->second.first.get(key);
  }
  return "";
}

Baggage JaegerLogger::get_all_baggage(const std::string &trace_id) {
  cleanup_expired_baggage();
  const auto it = g_trace_baggage.find(trace_id);
  if (it != g_trace_baggage.end()) {
    return it->second.first;
  }
  return Baggage();
}

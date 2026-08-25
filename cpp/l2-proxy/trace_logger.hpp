#ifndef TRACE_LOGGER_HPP
#define TRACE_LOGGER_HPP

#include "nlohmann/json.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <iomanip>
#include <mutex>
#include <queue>
#include <random>
#include <ranges>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>

// Forward declarations to avoid transitive httplib include
class HttpClientPool;

// Tracing constants
inline constexpr size_t g_tracing_max_queue_size =
    10000; // Maximum spans in queue before dropping
inline constexpr int g_tracing_default_batch_size =
    50; // Default batch size for sending
inline constexpr int g_tracing_send_interval_ms =
    1000;                                       // Default flush interval in ms
inline constexpr int g_tracing_max_retries = 3; // Maximum retry attempts
inline constexpr int g_tracing_retry_base_delay_ms =
    100; // Base delay for exponential backoff
inline constexpr int g_tracing_send_timeout_ms =
    5000; // HTTP send timeout in ms

// Structure to hold extracted traceparent info
struct TraceInfo {
  std::string m_trace_id;
  std::string m_parent_span_id;
  std::string m_current_span_id;
  bool m_sampled;
  bool m_valid;

  TraceInfo() : m_sampled(true), m_valid(false) {}
};

// Structure for W3C Baggage (key-value pairs propagated across services)
struct Baggage {
  std::unordered_map<std::string, std::string> m_items;

  void set(const std::string &key, const std::string &value) {
    m_items[key] = value;
  }

  std::string get(const std::string &key) const {
    auto it = m_items.find(key);
    return (it != m_items.end()) ? it->second : "";
  }

  bool contains(const std::string &key) const {
    return m_items.find(key) != m_items.end();
  }

  size_t size() const { return m_items.size(); }

  // Serialize to W3C Baggage header format
  std::string to_header() const {
    std::string result;
    for (const auto &[key, value] : m_items) {
      if (!result.empty())
        result += ",";
      // URL encode key and value
      result.append(key).append("=").append(value);
    }
    return result;
  }

  // Parse from W3C Baggage header format
  static Baggage from_header(const std::string &header) {
    Baggage baggage;
    if (header.empty())
      return baggage;

    for (const auto item : header | std::views::split(',')) {
      const std::string_view item_sv(item.begin(), item.end());
      const size_t eq_pos = item_sv.find('=');
      if (eq_pos != std::string_view::npos) {
        std::string key(item_sv.substr(0, eq_pos));
        std::string value(item_sv.substr(eq_pos + 1));
        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        baggage.set(key, value);
      }
    }

    return baggage;
  }
};

// Extract trace_id, parent_span_id from traceparent header
// Returns empty TraceInfo if parsing fails
TraceInfo extract_trace_info(std::string_view traceparent);

class JaegerLogger {
private:
  std::string m_jaeger_url;
  prometheus::Counter &m_tracing_spans_sent_counter;
  prometheus::Counter &m_tracing_spans_failed_counter;
  prometheus::Gauge &m_tracing_queue_size_gauge;
  prometheus::Gauge &m_tracing_last_send_duration_gauge;

  prometheus::Histogram &m_tracing_send_latency_histogram;
  prometheus::Histogram &m_tracing_queue_time_histogram;

  std::unique_ptr<HttpClientPool> m_http_client_pool;

  struct SpanData {
    std::string m_trace_id, m_span_id, m_parent_id, m_name, m_service_name;
    uint64_t m_start_us, m_end_us;
    uint64_t m_enqueue_time_us; // For queue time measurement
    nlohmann::json m_attributes;
    Baggage m_baggage; // W3C Baggage for cross-service correlation
  };

  std::deque<SpanData> m_span_queue;
  std::mutex m_queue_mutex;
  std::thread m_sender_thread;
  std::atomic<bool> m_stop_sender{false};

  // Configuration
  size_t m_batch_size;
  int m_flush_interval_ms;
  double m_sample_rate; // 0.0-1.0, 1.0 = 100% sampling

  // Retry state
  std::atomic<int> m_consecutive_failures{0};

private:
  bool send_batch(const std::vector<SpanData> &batch);

  // Fast random hex generation (thread-local, no re-initialization)
  static std::string random_hex_fast(size_t len);

public:
  // Build a single Zipkin v2 span JSON object (Jaeger /api/v2/spans schema).
  // Pure helper (no member state) so it is unit-testable without a registry.
  static nlohmann::json build_span_json(const std::string &trace_id,
                                        const std::string &span_id,
                                        const std::string &parent_id,
                                        const std::string &name,
                                        uint64_t start_us, uint64_t end_us,
                                        const std::string &service_name,
                                        const nlohmann::json &attributes) {
    nlohmann::json tags = nlohmann::json::object();
    for (const auto &el : attributes.items()) {
      if (el.value().is_string()) {
        tags[el.key()] = el.value();
      } else {
        tags[el.key()] = el.value().dump();
      }
    }

    nlohmann::json span = nlohmann::json::object();
    span["id"] = span_id;
    span["traceId"] = trace_id;
    span["name"] = name;
    span["timestamp"] = start_us;
    span["duration"] = end_us - start_us;
    span["localEndpoint"] = {{"serviceName", service_name}};
    span["tags"] = tags;

    if (!parent_id.empty()) {
      span["parentId"] = parent_id;
    }

    return span;
  }

  JaegerLogger(const std::string &endpoint, prometheus::Counter &spans_sent,
               prometheus::Counter &spans_failed, prometheus::Gauge &queue_size,
               prometheus::Gauge &last_send_duration,
               prometheus::Histogram &send_latency,
               prometheus::Histogram &queue_time,
               size_t batch_size = g_tracing_default_batch_size,
               int flush_interval_ms = g_tracing_send_interval_ms,
               double sample_rate = 1.0);
  ~JaegerLogger();

  std::string generate_trace_id();
  std::string generate_span_id();

  // W3C Trace Context helpers
  std::string generate_traceparent(std::string_view trace_id,
                                   std::string_view span_id,
                                   bool sampled = true);

  // Parse traceparent header: "00-{trace-id}-{parent-id}-{flags}"
  // Pure function: also available to the static extract_trace_info() helper.
  static bool parse_traceparent(std::string_view traceparent,
                                std::string &trace_id,
                                std::string &parent_span_id, bool &sampled);

  void enqueue_span(const std::string &trace_id, const std::string &span_id,
                    const std::string &parent_id, const std::string &name,
                    uint64_t start_us, uint64_t end_us,
                    const std::string &service_name,
                    const nlohmann::json &attributes = {});

  void log_request(const std::string &method, const std::string &url,
                   int status_code, uint64_t start_us, uint64_t end_us,
                   const std::string &service_name,
                   const std::string &request_id = "",
                   const std::string &trace_id = "",
                   const std::string &span_id = "",
                   const std::string &parent_id = "",
                   const nlohmann::json &additional_attributes = {});

  // Simplified validation
  static bool validate_traceparent(std::string_view traceparent);

  void sender_loop();
  bool send_span(const std::string &trace_id, const std::string &span_id,
                 const std::string &parent_id, const std::string &name,
                 uint64_t start_us, uint64_t end_us,
                 const std::string &service_name,
                 const nlohmann::json &attributes = {});

  bool should_sample() const;

  // Check if sampling should be applied (with error flag - always sample
  // errors)
  bool should_sample(bool is_error) const;

  // Baggage propagation
  void set_baggage(const std::string &trace_id, const std::string &key,
                   const std::string &value);
  std::string get_baggage(const std::string &trace_id, const std::string &key);
  Baggage get_all_baggage(const std::string &trace_id);
};

#endif // TRACE_LOGGER_HPP

#ifndef L2_WORKER_HPP
#define L2_WORKER_HPP

#include "app_context.hpp"
#include "common_utils.hpp"
#include "dedup_cache.hpp"
#include "http_client.hpp"
#include "http_client_pool.hpp"
#include "nats_client.hpp"
#include "nlohmann/json.hpp"
#include "thread_pool_wrapper.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using json = nlohmann::json;

class L2Worker {
  // Circuit breaker for L2 server calls
  struct CircuitBreaker {
    enum class State : std::uint8_t { CLOSED = 0, OPEN = 1, HALF_OPEN = 2 };

    prometheus::Gauge *m_gauge = nullptr;
    std::atomic<State> m_state{State::CLOSED};
    std::atomic<int> m_failure_count{0};
    std::atomic<int> m_success_count{0};
    std::atomic<uint64_t> m_last_failure_time_us{0};

    static constexpr int g_failure_threshold = 5;
    static constexpr uint64_t g_open_timeout_us = 10'000'000; // 10 seconds
    static constexpr int g_half_open_success_threshold = 2;

    void set_gauge(prometheus::Gauge *gauge);
    bool allow_request();
    void record_success();
    void record_failure();
    std::string state_name() const;

  private:
    void update_gauge();
  };

private:
  std::unique_ptr<HttpClientPool> m_http_client_pool;
  std::unique_ptr<ThreadPoolWrapper> m_thread_pool;
  std::unique_ptr<NatsClient> m_nats_client;
  AppContext &m_ctx;
  std::vector<std::string> m_l2_server_urls;
  CircuitBreaker m_circuit_breaker;
  // Caches produced responses by request_id so a re-delivered NATS request
  // (reply lost during reconnect) is answered without a duplicate L2 call.
  DedupCache m_dedup_cache;

public:
  explicit L2Worker(AppContext &context);
  ~L2Worker();
  HttpResponse call_l2_server(const std::string &path, const std::string &body,
                              const std::string &traceparent = "",
                              const httplib::Headers &forwarded_headers = {},
                              const std::string &method = "POST",
                              const std::string &parent_span_id = "",
                              const std::string &l2_call_span_id = "",
                              const std::string &query = "");
  void run();
  bool is_nats_connected() const;
  static std::string extract_scheme_host_port(const std::string &url);

private:
  void extract_forwarded_headers(const json &request_data,
                                 httplib::Headers &forwarded_headers);
  std::string extract_l2_server_span_id(const std::string &l2_response);
  json prepare_response_headers(const HttpResponse &l2_http_response);
  bool is_l2_server_allowed(const std::string &path, std::string &selected_url);
  std::string construct_l2_url(const std::string &selected_url,
                               const std::string &path);
  // NATS mode methods
  void run_with_nats();
  void process_request_from_nats(const std::string &request_json,
                                 const std::string &reply_to);
  void send_nats_response(const std::string &reply_to,
                          const std::string &response_json);
  void send_nats_response(const std::string &reply_to,
                          const std::string &response_json,
                          const NatsHeaders &headers);
  void send_nats_response_impl(const std::string &reply_to,
                               const std::string &response_json,
                               const NatsHeaders *headers = nullptr);

  bool validate_l2_server_access(const std::string &path,
                                 std::string &selected_url);
  HttpResponse execute_l2_call_with_retry(
      const std::string &url, const std::string &query, const std::string &body,
      const std::string &traceparent, const httplib::Headers &forwarded_headers,
      const std::string &method, int &final_attempt);
  void record_l2_call_metrics(uint64_t start_us);

  struct RequestData {
    std::string m_request_id;
    std::string m_path;
    std::string m_query;
    std::string m_method;
    std::string m_body;
    std::string m_client_ip;
    std::string m_proxy_ip;
    std::string m_traceparent;
    std::string m_proxy_traceparent;
    std::string m_proxy_span_id;
    TraceContext m_trace_ctx;
    httplib::Headers m_forwarded_headers;
  };

  struct TracingSpans {
    std::string m_worker_process_span_id;
    std::string m_worker_process_parent_id;
    std::string m_l2_call_span_id;
    std::string m_traceparent_header;
    std::string m_setex_span_id;
  };

  struct L2Response {
    std::string m_body;
    httplib::Headers m_headers;
    int m_status_code;
    std::string m_l2_server_span_id;
  };

  struct ResponseData {
    std::string m_response_str;
    std::string m_content_type;
    bool m_is_binary;
    uint64_t m_timestamp_us;
  };

  // Pipeline stages
  bool parse_request_data(const std::string &request_json, json &request_data);
  RequestData extract_request_metadata(const json &request_data);
  TracingSpans create_tracing_spans(const TraceContext &parent_trace_ctx,
                                    const std::string &op_parent_span_id);
  L2Response execute_l2_call(const RequestData &metadata,
                             const TracingSpans &spans);
  ResponseData prepare_response_data(const L2Response &l2_response,
                                     const TracingSpans &spans);
};

#endif // L2_WORKER_HPP

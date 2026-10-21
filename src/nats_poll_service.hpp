#ifndef NATS_POLL_SERVICE_HPP
#define NATS_POLL_SERVICE_HPP

#include "app_context.hpp"
#include "common_utils.hpp"
#include "nats_client.hpp"
#include "retry_handler.hpp"
#include "trace_logger.hpp"
#include <atomic>
#include <functional>

class NatsPollService {
private:
  // The result of a poll: the reply and the worker's consume span id (read
  // from the NATS headers).
  struct PollOutcome {
    NatsReply m_reply;
    std::string m_consume_span_id;
  };

  // Polls for the reply until the deadline: reconnects, resends and records
  // the attempt metrics. Returns an empty reply when the deadline passed.
  [[nodiscard]] PollOutcome poll_until_deadline(const std::string &request_id,
                                                const std::string &request_json,
                                                int timeout_ms);
  AppContext &m_ctx;

  bool poll_ensure_connected(const std::string &request_id,
                             RetryHandler &reconnect_backoff,
                             bool &reconnect_logged);
  void poll_notify_resend(const std::string &request_id, bool &first_attempt,
                          bool &resend_logged) const;
  void poll_delay_for_empty_reply(const std::string &request_id,
                                  const std::string &last_error,
                                  bool &no_responders_logged);
  // Backoff sleep between poll attempts; records the delay in
  // l2_proxy_nats_poll_retry_wait_seconds so retry cost is visible next to
  // the pure NATS attempt latency.
  void poll_sleep_backoff(int delay_ms) const;

public:
  explicit NatsPollService(AppContext &ctx);

  std::string poll_response(const std::string &request_id,
                            const std::string &request_json,
                            int timeout_seconds, const TraceContext &trace_ctx);
};

#endif // NATS_POLL_SERVICE_HPP

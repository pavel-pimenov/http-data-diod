#ifndef NATS_POLL_SERVICE_HPP
#define NATS_POLL_SERVICE_HPP

#include "app_context.hpp"
#include "common_utils.hpp"
#include "retry_handler.hpp"
#include "trace_logger.hpp"
#include <atomic>
#include <functional>

class NatsPollService {
private:
  AppContext &m_ctx;

  bool poll_ensure_connected(const std::string &request_id,
                             RetryHandler &reconnect_backoff,
                             bool &reconnect_logged);
  void poll_notify_resend(const std::string &request_id, bool &first_attempt,
                          bool &resend_logged);
  void poll_delay_for_empty_reply(const std::string &request_id,
                                  const std::string &last_error,
                                  bool &no_responders_logged);

public:
  explicit NatsPollService(AppContext &ctx);

  std::string poll_response(const std::string &request_id,
                            const std::string &request_json,
                            int timeout_seconds, const TraceContext &trace_ctx);
};

#endif // NATS_POLL_SERVICE_HPP

#ifndef NATS_POLL_SERVICE_HPP
#define NATS_POLL_SERVICE_HPP

#include "app_context.hpp"
#include "common_utils.hpp"
#include "trace_logger.hpp"
#include <atomic>
#include <functional>

class NatsPollService {
private:
  AppContext &m_ctx;

public:
  explicit NatsPollService(AppContext &ctx);

  std::string poll_response(const std::string &request_id,
                            const std::string &request_json,
                            int timeout_seconds, const TraceContext &trace_ctx);
};

#endif // NATS_POLL_SERVICE_HPP

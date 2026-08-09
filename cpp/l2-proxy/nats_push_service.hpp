#ifndef NATS_PUSH_SERVICE_HPP
#define NATS_PUSH_SERVICE_HPP

#include "app_context.hpp"
#include "common_utils.hpp"
#include "logger.hpp"
#include "trace_logger.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

class NatsPushService {
private:
  AppContext &m_ctx;

public:
  explicit NatsPushService(AppContext &ctx);

  std::string push_request(nlohmann::json request_data,
                           const std::string &trace_id,
                           const std::string &span_id,
                           const TraceContext &trace_ctx);
};

#endif // NATS_PUSH_SERVICE_HPP

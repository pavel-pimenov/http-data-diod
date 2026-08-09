#ifndef RESPONSE_BUILDER_HPP
#define RESPONSE_BUILDER_HPP

#include "app_context.hpp"
#include "common_utils.hpp"
#include "httplib/httplib.h"
#include "nlohmann/json.hpp"
#include "stats_logger.hpp"
#include "trace_logger.hpp"
#include <string>

// Build HTTP response from backend response data (parsed JSON)
void set_response_content(httplib::Response &res,
                          const nlohmann::json &response_data,
                          const std::string &request_id,
                          const TraceContext &trace_ctx,
                          const std::string &method, const std::string &path,
                          uint64_t start_us, AppContext &ctx,
                          StatsLogger *stats_logger);

#endif // RESPONSE_BUILDER_HPP
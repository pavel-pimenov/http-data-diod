#ifndef SERVER_HANDLER_HPP
#define SERVER_HANDLER_HPP

#include "httplib/httplib.h"

#include "app_context.hpp"
#include "nlohmann/json.hpp"
#include "trace_logger.hpp"
#include <memory>

using json = nlohmann::json;

class ServerHandler {
private:
  AppContext &m_ctx;
  void send_response_with_trace(const httplib::Request &req,
                                httplib::Response &res, json response_json,
                                uint64_t start_us);

  // Serves the embedded favicon.ico for the binary GET integrity test.
  void handle_favicon(httplib::Response &res) const;

  // Test-mode random delay (L2_TEST_RESPONSE_DELAY_MS) that desynchronizes
  // response order from request order so the correlation test can exercise
  // response mixing. No-op when the env var is not set.
  void apply_test_delay() const;

public:
  explicit ServerHandler(AppContext &ctx);
  void handle_post(const httplib::Request &req, httplib::Response &res);
  void handle_get(const httplib::Request &req, httplib::Response &res);
};

#endif // SERVER_HANDLER_HPP

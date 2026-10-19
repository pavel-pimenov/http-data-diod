#ifndef TEST_HTTP_SUPPORT_HPP
#define TEST_HTTP_SUPPORT_HPP

#include "httplib/httplib.h"

#include <string>
#include <thread>

// Helpers shared by the unit tests that need a mock HTTP endpoint.

namespace test_support {

// RAII fixture for a mock HTTP server: it binds an ephemeral port, serves in
// a background thread and stops the thread in the destructor. The explicit
// lifetime is the point: a test that fails a REQUIRE aborts the case before
// the manual stop()/join() at the end of the body would run, and the server
// object would be destroyed while its thread was still serving.
//
// Usage: register the routes on http(), then start().
class MockHttpServer {
public:
  MockHttpServer() = default;
  MockHttpServer(const MockHttpServer &) = delete;
  MockHttpServer &operator=(const MockHttpServer &) = delete;
  ~MockHttpServer() { stop(); }

  httplib::Server &http() { return m_server; }
  [[nodiscard]] int port() const { return m_port; }
  [[nodiscard]] std::string base_url() const {
    return "http://127.0.0.1:" + std::to_string(m_port);
  }

  // A non-zero fixed_port rebinds a previously reserved port (used to bring an
  // endpoint back on the same address).
  void start(int fixed_port = 0) {
    if (fixed_port > 0) {
      m_server.bind_to_port("127.0.0.1", fixed_port);
      m_port = fixed_port;
    } else {
      m_port = m_server.bind_to_any_port("127.0.0.1");
    }
    m_thread = std::thread([this] { m_server.listen_after_bind(); });
    m_server.wait_until_ready();
  }

  void stop() {
    m_server.stop();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

private:
  httplib::Server m_server;
  int m_port = 0;
  std::thread m_thread;
};

} // namespace test_support

#endif // TEST_HTTP_SUPPORT_HPP

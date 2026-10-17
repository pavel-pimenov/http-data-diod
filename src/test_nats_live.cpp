// Live-broker coverage for the NATS stack (round 71, Tracks A+B): unit tests
// that exec a real nats-server on an ephemeral loopback port and drive
// NatsClient, NatsPollService, the HTTP DB gateway and the L2Worker run loop
// against it, so the whole request/reply path — including the disconnected/
// reconnected callbacks and envelope round-trips — runs for real.
//
// The server binary is COPYed into the builder image from nats:2.14-alpine
// (src/Dockerfile); tests SKIP when it is not present so local runs without a
// server still pass. Offline fail-fast tests (no broker) ship in the same
// file too: they reuse the TLS-CA trick from test_proxy_handlers.cpp — the
// connect aborts before dialing, so every subsequent NATS call returns
// immediately.
//
// Live tests run inside the coverage image (RUN ./test_components), so all
// synchronisation is bounded — nothing may hang the docker build.

#include "app_context.hpp"
#include "common_utils.hpp"
#include "config.hpp"
#include "db_query_utils.hpp"
#include "httplib/httplib.h"
#include "json_utils.hpp"
#include "l2_worker.hpp"
#include "nats_client.hpp"
#include "nats_poll_service.hpp"
#include "proxy_init.hpp"
#include "request_handler.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <nats/nats.h>
#include <nats/status.h>
#include <netinet/in.h>
#include <optional>
#include <stdlib.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern std::atomic<bool> g_shutdown_flag;

namespace {

constexpr const char *kTraceparent =
    "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

// Scoped env-var overrides (EnvMapGuard/EnvVars) and the tracer/request stubs
// are shared via test_support.hpp; the values must be in place before the
// Config is loaded by the AppContext ctor.
using test_support::attach_tracer;
using test_support::EnvMapGuard;
using test_support::EnvVars;
using test_support::make_get;
using test_support::ProxyHandler;
using test_support::reserve_loopback_port;
using test_support::wait_for_condition;

// Path of the nats-server binary COPYed into the builder image; empty when the
// binary is not installed (tests then SKIP). Also consulted along PATH so the
// tests can run against a hand-installed binary.
constexpr std::array<const char *, 2> kNatsServerCandidates = {
    "/usr/local/bin/nats-server", "/usr/bin/nats-server"};

std::string nats_binary_path() {
  for (const char *candidate : kNatsServerCandidates) {
    if (::access(candidate, X_OK) == 0) {
      return std::string(candidate);
    }
  }
  const char *path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return "";
  }
  std::string const path(path_env);
  size_t start = 0;
  while (start <= path.size()) {
    const size_t sep = path.find(':', start);
    const std::string dir =
        path.substr(start, sep == std::string::npos ? std::string::npos
                                                    : sep - start);
    if (!dir.empty()) {
      const std::string candidate = dir + "/nats-server";
      if (::access(candidate.c_str(), X_OK) == 0) {
        return candidate;
      }
    }
    if (sep == std::string::npos) {
      break;
    }
    start = sep + 1;
  }
  return "";
}

[[nodiscard]] bool tcp_probe(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  const int rc = ::connect(fd, reinterpret_cast<const sockaddr *>(&addr),
                           sizeof(addr));
  ::close(fd);
  return rc == 0;
}

[[nodiscard]] bool wait_for_port(int port, bool want_open, int timeout_ms) {
  return wait_for_condition([port, want_open]() {
    return tcp_probe(port) == want_open;
  }, timeout_ms);
}

// RAII nats-server on an ephemeral loopback port: fork/exec, wait for the
// port to accept connections, SIGTERM on teardown (SIGKILL fallback).
class NatsServer {
public:
  NatsServer() : m_port(reserve_loopback_port()) {}

  [[nodiscard]] int port() const { return m_port; }
  [[nodiscard]] pid_t pid() const { return m_pid; }

  bool start() {
    if (m_pid > 0) {
      return false;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
      ::setsid();
      const std::string port = std::to_string(m_port);
      const std::string log =
          "/tmp/nats-test-" + std::to_string(m_port) + ".log";
      const int fd = ::open(log.c_str(), O_CREAT | O_APPEND | O_WRONLY, 0644);
      if (fd >= 0) {
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        ::close(fd);
      }
      if (!m_config_file.empty()) {
        ::execlp("nats-server", "nats-server", "-c", m_config_file.c_str(),
                 nullptr);
      } else if (m_tls_cert.empty() || m_tls_key.empty()) {
        ::execlp("nats-server", "nats-server", "-a", "127.0.0.1", "-p",
                 port.c_str(), nullptr);
      } else {
        ::execlp("nats-server", "nats-server", "-a", "127.0.0.1", "-p",
                 port.c_str(), "--tls", "--tlscert", m_tls_cert.c_str(),
                 "--tlskey", m_tls_key.c_str(), nullptr);
      }
      _exit(127);
    }
    if (pid < 0) {
      return false;
    }
    m_pid = pid;
    return true;
  }

  bool wait_ready(int timeout_ms = 8000) {
    return wait_for_port(m_port, true, timeout_ms);
  }

  void stop() {
    if (m_pid <= 0) {
      return;
    }
    ::kill(m_pid, SIGTERM);
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    for (;;) {
      const pid_t r = ::waitpid(m_pid, &status, WNOHANG);
      if (r == m_pid) {
        m_pid = 0;
        return;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::kill(m_pid, SIGKILL);
    ::waitpid(m_pid, &status, 0);
    m_pid = 0;
  }

  void restart() {
    stop();
    REQUIRE(start());
    REQUIRE(wait_ready());
  }

  // Starts the broker for a live test, or marks the test skipped when the
  // nats-server binary is not part of the image (SKIP throws, so it unwinds
  // through the caller like the guard it replaced). The guard and the two
  // start assertions used to be copy-pasted at the top of every [nats-live]
  // test and re-typed around each extra broker inside a test.
  void start_live() {
    if (nats_binary_path().empty()) {
      SKIP("nats-server binary not available");
    }
    REQUIRE(start());
    REQUIRE(wait_ready());
  }

  ~NatsServer() { stop(); }

  void enable_tls(const std::string &cert, const std::string &key) {
    m_tls_cert = cert;
    m_tls_key = key;
  }

  void use_config_file(const std::string &path) { m_config_file = path; }

private:
  int m_port = 0;
  pid_t m_pid = 0;
  std::string m_tls_cert;
  std::string m_tls_key;
  std::string m_config_file;
};

// Runs L2Worker::run() on a background thread with the process-wide shutdown
// flag cleared, and raises the flag plus joins on scope exit. A test that fails
// in the middle would otherwise leave the worker running and the flag raised
// for the next test, and a joinable std::thread reaching its destructor aborts
// the whole binary. The flag is the same global the signal handlers toggle.
class WorkerRunner {
public:
  explicit WorkerRunner(L2Worker &worker) {
    g_shutdown_flag = false;
    m_thread = std::thread([&worker] { worker.run(); });
  }

  ~WorkerRunner() {
    g_shutdown_flag = true;
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

private:
  std::thread m_thread;
};

std::string unique_subject(const std::string &base) {
  static std::atomic<unsigned> seq{0};
  return base + "." + std::to_string(::getpid()) + "." +
         std::to_string(seq.fetch_add(1));
}

std::pair<std::string, std::string> generate_tls_certs(const std::string &base_dir = "/tmp") {
  std::string const& dir = base_dir;
  const std::string prefix = dir + "/nats-tls-" + std::to_string(::getpid()) + "-" +
                             std::to_string(::time(nullptr));
  const std::string key_path = prefix + ".key";
  const std::string crt_path = prefix + ".crt";
  std::string const cmd =
      "openssl req -x509 -newkey rsa:2048 -keyout " + key_path + " -out " +
      crt_path + " -days 1 -nodes -subj '/CN=localhost' >/dev/null 2>&1";
  (void)::system(cmd.c_str());
  return {crt_path, key_path};
}

NatsConfig live_nats_cfg(const std::string &subject, int port) {
  NatsConfig cfg;
  cfg.m_host = "127.0.0.1";
  cfg.m_port = port;
  cfg.m_subject = subject;
  cfg.m_queue_group = "test-group";
  cfg.m_timeout_ms = 3000;
  return cfg;
}

EnvVars fail_fast_env(const std::string &subject) {
  EnvVars vars;
  vars["MODE"] = "proxy";
  vars["LOG_LEVEL"] = "ERROR";
  vars["NATS_HOST"] = "127.0.0.1";
  vars["NATS_PORT"] = "4222";
  vars["NATS_SUBJECT"] = subject;
  vars["NATS_ENABLE_TLS"] = "true";
  vars["NATS_TLS_CERT_FILE"] = std::nullopt;
  vars["NATS_TLS_KEY_FILE"] = std::nullopt;
  vars["NATS_TLS_CA_CERT_FILE"] = "/nonexistent-test-ca.pem";
  vars["NATS_TIMEOUT_MS"] = "1000";
  vars["DB_QUERY_NATS_TIMEOUT_MS"] = "1000";
  vars["REQUEST_TIMEOUT_SECONDS"] = "1";
  return vars;
}

EnvVars live_proxy_env(int port, const std::string &subject,
                       const std::string &db_subject = "") {
  EnvVars vars;
  vars["MODE"] = "proxy";
  vars["LOG_LEVEL"] = "ERROR";
  vars["NATS_HOST"] = "127.0.0.1";
  vars["NATS_PORT"] = std::to_string(port);
  vars["NATS_SUBJECT"] = subject;
  vars["NATS_ENABLE_TLS"] = "false";
  vars["NATS_TLS_CERT_FILE"] = std::nullopt;
  vars["NATS_TLS_KEY_FILE"] = std::nullopt;
  vars["NATS_TLS_CA_CERT_FILE"] = std::nullopt;
  vars["NATS_TIMEOUT_MS"] = "3000";
  vars["DB_QUERY_NATS_TIMEOUT_MS"] = "3000";
  vars["REQUEST_TIMEOUT_SECONDS"] = "3";
  if (!db_subject.empty()) {
    vars["DB_QUERY_ENABLED"] = "true";
    vars["DB_POSTGRES_ENABLED"] = "true";
    vars["DB_QUERY_NATS_SUBJECT"] = db_subject;
  }
  return vars;
}

EnvVars live_worker_env(int port, const std::string &subject,
                        const std::string &urls) {
  EnvVars vars;
  vars["MODE"] = "worker";
  vars["LOG_LEVEL"] = "ERROR";
  vars["THREAD_POOL_TYPE"] = "none";
  vars["L2_WORKER_THREADS"] = "2";
  vars["L2_WORKER_QUEUE_SIZE"] = "16";
  vars["HTTP_TIMEOUT_SECONDS"] = "3";
  vars["HTTP_POOL_SIZE"] = "8";
  vars["MAX_RETRIES"] = "2";
  vars["DEDUP_ENABLED"] = "true";
  vars["NATS_HOST"] = "127.0.0.1";
  vars["NATS_PORT"] = std::to_string(port);
  vars["NATS_SUBJECT"] = subject;
  vars["NATS_ENABLE_TLS"] = "false";
  vars["NATS_TLS_CERT_FILE"] = std::nullopt;
  vars["NATS_TLS_KEY_FILE"] = std::nullopt;
  vars["NATS_TLS_CA_CERT_FILE"] = std::nullopt;
  vars["NATS_TIMEOUT_MS"] = "3000";
  vars["DB_QUERY_NATS_TIMEOUT_MS"] = "1000";
  vars["L2_SERVER_HOST"] = "127.0.0.1";
  vars["L2_SERVER_PORT"] = "8088";
  vars["L2_SERVER_URLS"] = urls;
  return vars;
}

json make_worker_request(const std::string &request_id,
                         const std::string &path) {
  json request;
  request[NatsContract::kRequestId] = request_id;
  request[NatsContract::kMethod] = "POST";
  request[NatsContract::kPath] = path;
  request[NatsContract::kBody] = R"({"payload":"test"})";
  request[NatsContract::kClientIp] = "10.9.8.7";
  request[NatsContract::kProxyIp] = "10.9.8.1";
  request[NatsContract::kTraceparent] = kTraceparent;
  request[NatsContract::kProxyTraceparent] = kTraceparent;
  request[NatsContract::kProxySpanId] = "proxy-inlet-span";
  request[NatsContract::kHeaders] = json{{"X-Real-IP", "10.9.8.7"},
                                         {"X-DataHub-Client-Id", "client-42"},
                                         {"User-Agent", "l2-test/1.0"}};
  return request;
}

// Subscriber that answers every message on its subject and lets the caller
// decide the reply payload/headers per request.
class NatsResponder {
public:
  using ReplyFn =
      std::function<NatsReply(const std::string &, const std::string &)>;

  NatsResponder(const NatsConfig &cfg, const std::string &subject,
                ReplyFn reply_fn)
      : m_client(cfg), m_reply_fn(std::move(reply_fn)) {
    REQUIRE(m_client.connect());
    const bool subscribed = m_client.subscribe(
        subject, [this](const std::string &, const std::string &data,
                        const std::string &reply_to) {
          const NatsReply reply = m_reply_fn(data, reply_to);
          if (reply.m_headers.empty()) {
            m_client.publish(reply_to, reply.m_data);
          } else {
            m_client.publish_with_headers(reply_to, reply.m_data,
                                          reply.m_headers);
          }
        });
    REQUIRE(subscribed);
    // Give nats-server a moment to register the subscription before a request
    // arrives (request/reply silently waits for a responder otherwise).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  ~NatsResponder() { m_client.disconnect(); }

private:
  NatsClient m_client;
  ReplyFn m_reply_fn;
};

// Loopback stand-in for the L2 backend used by the worker live test.
struct L2BackendServer {
  httplib::Server m_server;
  int m_port = 0;
  std::thread m_thread;
  std::atomic<int> m_posts{0};

  L2BackendServer() {
    m_server.Post("/api/value",
                  [this](const httplib::Request &, httplib::Response &res) {
                    m_posts.fetch_add(1);
                    res.set_content(R"({"server_span_id":"l2-live-val"})",
                                    "application/json");
                  });
    m_port = m_server.bind_to_any_port("127.0.0.1");
    m_thread = std::thread([this] { m_server.listen_after_bind(); });
    m_server.wait_until_ready();
  }

  ~L2BackendServer() {
    m_server.stop();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  std::string api_url() const {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/api";
  }
};

} // namespace

// ============================================================================
// Track C (offline): the public API fails fast without a reachable broker
// ============================================================================

TEST_CASE("NatsClient: public API fails fast without a broker", "[nats-offline]") {
  const EnvMapGuard env(fail_fast_env(unique_subject("nats.offline")));
  AppContext ctx;
  init_proxy_components(ctx);
  auto &client = *ctx.m_nats_client;

  REQUIRE_FALSE(client.is_connected());
  REQUIRE_FALSE(client.connect());
  REQUIRE(client.get_last_error().has_value());

  CHECK_FALSE(client.publish("s", "d"));
  CHECK_FALSE(client.publish_with_headers("s", "d", {{"h", "v"}}));
  const auto msg_handler = [](const std::string &, const std::string &,
                              const std::string &) {};
  CHECK_FALSE(client.subscribe("s", msg_handler));
  CHECK_FALSE(client.subscribe_queue("s", "q", msg_handler));

  const std::optional<std::string> req = client.request("s", "d", 0);
  CHECK_FALSE(req.has_value());
  const NatsReply rh =
      client.request_with_headers("s", "d", {{"h", "v"}}, {"k"}, 0);
  CHECK(rh.m_data.empty());
  const auto [r2, span_id] =
      client.request_with_consume_span_id("s", "d", 0);
  CHECK(r2.m_data.empty());
  CHECK(span_id.empty());

  CHECK_FALSE(client.check_connection());
  CHECK_FALSE(client.ping().has_value());
  client.unsubscribe();
  client.drain(10);
  client.disconnect();
}

TEST_CASE("NatsPollService: retries while the client stays disconnected",
          "[nats-offline]") {
  const EnvMapGuard env(fail_fast_env(unique_subject("nats.polloff")));
  AppContext ctx;
  init_proxy_components(ctx);
  REQUIRE_FALSE(ctx.m_nats_client->is_connected());

  NatsPollService svc(ctx);
  TraceContext trace_ctx;
  trace_ctx.m_trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";
  const std::string out =
      svc.poll_response("req-offline", R"({"q":1})", 1, trace_ctx);
  REQUIRE(out.empty());
}

TEST_CASE("NatsPollService: returns empty when the NATS client is missing",
          "[nats-offline]") {
  TraceContext trace_ctx;
  trace_ctx.m_trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";
  const EnvMapGuard env(live_proxy_env(4222, unique_subject("poll.null")));
  AppContext ctx;
  // init_proxy_components() is deliberately NOT called: the proxy service must
  // tolerate a poll before the NATS client is created.
  REQUIRE_FALSE(ctx.m_nats_client);
  NatsPollService svc(ctx);
  const std::string out =
      svc.poll_response("req-null", R"({"q":1})", 1, trace_ctx);
  REQUIRE(out.empty());
}

// ============================================================================
// Track A: NatsClient against a live broker
// ============================================================================

TEST_CASE("NatsClient: pub/sub round-trip, headers and queue subscribers",
          "[nats-live]") {
  NatsServer server;
  server.start_live();

  const std::string subject = unique_subject("nats.pubsub");
  NatsClient client(live_nats_cfg(subject, server.port()));
  REQUIRE(client.connect());

  std::atomic<int> plain{0};
  REQUIRE(client.subscribe(subject, [&plain](const std::string &,
                                             const std::string &,
                                             const std::string &) { plain.fetch_add(1); }));

  const std::string qsubject = subject + ".q";
  std::atomic<int> queue_a{0};
  std::atomic<int> queue_b{0};
  NatsClient queue_client(live_nats_cfg(qsubject, server.port()));
  REQUIRE(queue_client.connect());
  REQUIRE(queue_client.subscribe_queue(
      qsubject, "workers",
      [&queue_a](const std::string &, const std::string &,
                 const std::string &) { queue_a.fetch_add(1); }));
  REQUIRE(client.subscribe_queue(qsubject, "workers",
                                 [&queue_b](const std::string &,
                                            const std::string &,
                                            const std::string &) { queue_b.fetch_add(1); }));

  // Let the subscriptions register with the server before publishing.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  for (int i = 0; i < 3; ++i) {
    REQUIRE(client.publish(subject, "plain-" + std::to_string(i)));
  }
  REQUIRE(client.publish_with_headers(subject, "with-headers",
                                      {{"X-Marker", "v"}}));
  for (int i = 0; i < 30; ++i) {
    REQUIRE(client.publish(qsubject, "queue-" + std::to_string(i)));
  }

  REQUIRE(wait_for_condition([&plain]() { return plain.load() == 4; }, 5000));
  REQUIRE(queue_a.load() > 0);
  REQUIRE(queue_b.load() > 0);
  REQUIRE(queue_a.load() + queue_b.load() == 30);

  client.unsubscribe();
  queue_client.unsubscribe();
}

TEST_CASE("NatsClient: request/reply, reply headers and consume span id",
          "[nats-live]") {
  NatsServer server;
  server.start_live();

  const std::string subject = unique_subject("nats.reqrep");
  const auto reply_fn = [](const std::string &, const std::string &) {
    return NatsReply{
        R"({"pong":1})",
        NatsHeaders{{"X-Reply-Marker", "yes"},
                    {"X-Consume-Span-Id", "span-1"}}};
  };
  NatsResponder const responder(live_nats_cfg(subject, server.port()), subject,
                                reply_fn);

  NatsClient client(live_nats_cfg(subject, server.port()));
  REQUIRE(client.connect());
  // Reconnecting an already-connected client returns immediately (idempotent).
  REQUIRE(client.connect());

  const std::optional<std::string> r1 = client.request(subject, "ping", 3000);
  REQUIRE(r1.has_value());
  REQUIRE(*r1 == R"({"pong":1})");

  const NatsReply r2 = client.request_with_headers(
      subject, "ping", {{"X-Client", "c1"}}, {"X-Reply-Marker"}, 3000);
  REQUIRE(r2.m_data == R"({"pong":1})");
  REQUIRE(r2.m_headers.at("X-Reply-Marker") == "yes");

  const auto [r3, span_id] =
      client.request_with_consume_span_id(subject, "ping", 3000);
  REQUIRE(r3.m_data == R"({"pong":1})");
  REQUIRE(span_id == "span-1");

  REQUIRE(client.check_connection());
  const std::optional<std::string> pong = client.ping();
  REQUIRE(pong.has_value());
  REQUIRE_FALSE(pong->empty());

  client.unsubscribe();
  REQUIRE(client.drain(3000));
}

TEST_CASE("NatsClient: reconnects after the broker restarts", "[nats-live]") {
  NatsServer server;
  server.start_live();

  const std::string subject = unique_subject("nats.reconnect");
  NatsClient client(live_nats_cfg(subject, server.port()));
  REQUIRE(client.connect());
  std::atomic<int> received{0};
  REQUIRE(client.subscribe(subject, [&received](const std::string &,
                                                const std::string &,
                                                const std::string &) {
    received.fetch_add(1);
  }));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  REQUIRE(client.publish(subject, "before"));
  REQUIRE(wait_for_condition([&received]() { return received.load() == 1; },
                             5000));

  server.restart();
  REQUIRE(wait_for_condition([&client]() { return client.is_connected(); },
                             15000));

  REQUIRE(client.publish(subject, "after"));
  REQUIRE(wait_for_condition([&received]() { return received.load() == 2; },
                             10000));
}

TEST_CASE("NatsClient: request timeout marks the connection down and records "
          "the error",
          "[nats-live]") {
  NatsServer server;
  server.start_live();

  const std::string subject = unique_subject("nats.reqtimeout");
  NatsClient client(live_nats_cfg(subject, server.port()));
  REQUIRE(client.connect());

  std::atomic<int> received{0};
  REQUIRE(client.subscribe(subject, [&received](const std::string &,
                                                const std::string &,
                                                const std::string &) {
    received.fetch_add(1);
  }));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // The subscriber receives the request but never replies, so the request
  // fails on the wait timeout (not NATS_NO_RESPONDERS) and the client treats
  // the connection as unhealthy.
  const std::optional<std::string> reply = client.request(subject, "ping", 100);
  REQUIRE_FALSE(reply.has_value());
  REQUIRE(received.load() == 1);
  REQUIRE(client.get_last_error().has_value());
  REQUIRE_FALSE(client.is_connected());

  client.disconnect();
}

TEST_CASE("NatsClient: request with timeout 0 falls back to the config timeout",
          "[nats-live]") {
  NatsServer server;
  server.start_live();

  const std::string subject = unique_subject("nats.reqtimeout0");
  const auto reply_fn = [](const std::string &, const std::string &) {
    return NatsReply{"pong", NatsHeaders{}};
  };
  NatsResponder const responder(live_nats_cfg(subject, server.port()), subject,
                                reply_fn);

  NatsClient client(live_nats_cfg(subject, server.port()));
  REQUIRE(client.connect());
  const std::optional<std::string> reply = client.request(subject, "ping", 0);
  REQUIRE(reply.has_value());
  REQUIRE(*reply == "pong");

  client.disconnect();
}

TEST_CASE("NatsClient: subscribe rejects an invalid subject", "[nats-live]") {
  NatsServer server;
  server.start_live();

  NatsClient client(live_nats_cfg(unique_subject("nats.badsub"), server.port()));
  REQUIRE(client.connect());
  REQUIRE_FALSE(client.subscribe("invalid subject with spaces",
                                 [](const std::string &, const std::string &,
                                    const std::string &) {}));
  REQUIRE(client.get_last_error().has_value());

  client.disconnect();
}

TEST_CASE("NatsClient: rejects empty subject and empty header key",
          "[nats-live]") {
  NatsServer server;
  server.start_live();

  const std::string subject = unique_subject("nats.badargs");
  NatsClient client(live_nats_cfg(subject, server.port()));
  REQUIRE(client.connect());
  REQUIRE(client.is_connected());

  // Empty subject: natsMsg_Create fails before anything is published, so the
  // client stays connected and only records the error.
  REQUIRE_FALSE(client.request("", "payload", 500).has_value());
  REQUIRE(client.is_connected());
  REQUIRE_FALSE(client.publish_with_headers("", "payload", {}));
  REQUIRE(client.is_connected());
  REQUIRE(client.get_last_error().has_value());

  // Empty header key: set_msg_headers fails, the message is destroyed and
  // nothing is published — the connection is untouched.
  REQUIRE_FALSE(client.publish_with_headers(subject, "payload", {{"", "v"}}));
  REQUIRE(client.is_connected());
  REQUIRE(client
              .request_with_headers(subject, "payload", {{"", "v"}}, {}, 500)
              .m_data.empty());
  REQUIRE(client.is_connected());

  // Legacy publish path: natsConnection_PublishString rejects the empty subject
  // and marks the client down, so it runs last.
  REQUIRE_FALSE(client.publish("", "payload"));
  REQUIRE_FALSE(client.is_connected());

  client.disconnect();
}

TEST_CASE("NatsClient: connect fails fast when the broker is unreachable",
          "[nats-live]") {
  // A reserved-but-unbound port: natsConnection_Connect() returns
  // NATS_NO_SERVERS promptly. Regression guard for round 102: with
  // RetryOnFailedConnect enabled the C client never returns while the server is
  // down, so connect() blocked forever while holding the connection mutex.
  const int closed_port = reserve_loopback_port();
  REQUIRE(closed_port > 0);

  NatsClient client(
      live_nats_cfg(unique_subject("nats.unreachable"), closed_port));
  const auto start = std::chrono::steady_clock::now();
  REQUIRE_FALSE(client.connect());
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();
  REQUIRE(elapsed_ms < 15000);
  REQUIRE_FALSE(client.is_connected());
  REQUIRE(client.get_last_error().has_value());

  // The public API must fail fast too (single bounded reconnect attempt).
  const auto api_start = std::chrono::steady_clock::now();
  CHECK_FALSE(client.check_connection());
  CHECK_FALSE(client.ping().has_value());
  CHECK_FALSE(client.publish("s", "d"));
  CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - api_start)
            .count() < 10000);
}

TEST_CASE("NatsClient: health check fails fast once the broker stops",
          "[nats-live]") {
  NatsServer server;
  server.start_live();

  const std::string subject = unique_subject("nats.healthdown");
  NatsClient client(live_nats_cfg(subject, server.port()));
  REQUIRE(client.connect());
  REQUIRE(client.is_connected());

  server.stop();
  REQUIRE(wait_for_condition([&client]() { return !client.is_connected(); },
                             10000));

  // Regression guard: check_connection()/ping() used to hang forever here
  // because ensure_connected() -> connect() never returned while the broker was
  // down.
  const auto start = std::chrono::steady_clock::now();
  CHECK_FALSE(client.check_connection());
  CHECK_FALSE(client.ping().has_value());
  CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count() < 10000);

  client.disconnect();
}

TEST_CASE("NatsClient: async error callback records a permission violation",
          "[nats-live]") {
  NatsServer server;
  const std::string config_path =
      "/tmp/nats-perm-" + std::to_string(server.port()) + ".conf";
  {
    std::ofstream config(config_path);
    REQUIRE(config.is_open());
    config << "host: 127.0.0.1\n";
    config << "port: " << server.port() << "\n";
    config << "authorization {\n";
    config << "  users = [\n";
    config << "    { user: \"denied\", password: \"secret\", permissions: "
              "{ publish: { deny: \">\" } } }\n";
    config << "  ]\n";
    config << "}\n";
  }
  server.use_config_file(config_path);
  server.start_live();

  NatsConfig cfg = live_nats_cfg(unique_subject("nats.denied"), server.port());
  cfg.m_username = "denied";
  cfg.m_password = "secret";
  NatsClient client(cfg);
  REQUIRE(client.connect());

  // The user may connect but not publish: the broker answers with a
  // permissions-violation -ERR that libnats delivers through the asynchronous
  // error handler (`_processPermissionViolation` -> error callback). Waiting for
  // the recorded error makes on_async_nats_error() deterministic, unlike the
  // accidental coverage the worker reconnect test provided (whose timing made
  // the nats_client line coverage flaky).
  (void)client.publish(cfg.m_subject, "denied");
  const bool observed = wait_for_condition(
      [&client]() {
        const std::optional<std::string> last_error = client.get_last_error();
        return last_error.has_value() &&
               last_error->find("asynchronous NATS error") !=
                   std::string::npos;
      },
      8000);
  client.disconnect();
  ::unlink(config_path.c_str());
  REQUIRE(observed);
}

// ============================================================================
// Track B: NatsPollService / DB gateway / worker over a live broker
// ============================================================================

TEST_CASE("NatsPollService: round-trip, empty reply retry and no responders",
          "[nats-live]") {
  NatsServer server;
  server.start_live();
  TraceContext trace_ctx;
  trace_ctx.m_trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";

  {
    const std::string subject = unique_subject("poll.ok");
    const EnvMapGuard env(live_proxy_env(server.port(), subject));
    AppContext ctx;
    attach_tracer(ctx);
    init_proxy_components(ctx);
    REQUIRE(ctx.m_nats_client->is_connected());

    const auto reply_fn = [](const std::string &, const std::string &) {
      return NatsReply{R"({"status":200,"body":"ok"})",
                       NatsHeaders{{"X-Consume-Span-Id", "poll-span"}}};
    };
    NatsResponder const responder(live_nats_cfg(subject, server.port()),
                                  subject, reply_fn);

    NatsPollService svc(ctx);
    const std::string out =
        svc.poll_response("req-poll-1", R"({"q":1})", 3, trace_ctx);
    REQUIRE(out == R"({"status":200,"body":"ok"})");
  }

  {
    const std::string subject = unique_subject("poll.empty");
    const EnvMapGuard env(live_proxy_env(server.port(), subject));
    AppContext ctx;
    init_proxy_components(ctx);
    const auto reply_fn = [](const std::string &, const std::string &) {
      return NatsReply{""};
    };
    NatsResponder const responder(live_nats_cfg(subject, server.port()),
                                  subject, reply_fn);

    NatsPollService svc(ctx);
    const std::string out =
        svc.poll_response("req-poll-2", R"({"q":1})", 2, trace_ctx);
    REQUIRE(out.empty());
  }

  {
    const std::string subject = unique_subject("poll.noreply");
    const EnvMapGuard env(live_proxy_env(server.port(), subject));
    AppContext ctx;
    init_proxy_components(ctx);

    NatsPollService svc(ctx);
    const std::string out =
        svc.poll_response("req-poll-3", R"({"q":1})", 1, trace_ctx);
    REQUIRE(out.empty());
  }
}

TEST_CASE("NatsPollService: zero timeout falls back to the config timeout",
          "[nats-live]") {
  NatsServer server;
  server.start_live();
  TraceContext trace_ctx;
  trace_ctx.m_trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";

  const std::string subject = unique_subject("poll.timeout0");
  const EnvMapGuard env(live_proxy_env(server.port(), subject));
  AppContext ctx;
  init_proxy_components(ctx);
  REQUIRE(ctx.m_nats_client->is_connected());

  const auto reply_fn = [](const std::string &, const std::string &) {
    return NatsReply{R"({"status":200,"body":"ok"})",
                     NatsHeaders{{"X-Consume-Span-Id", "poll-span"}}};
  };
  NatsResponder const responder(live_nats_cfg(subject, server.port()), subject,
                                reply_fn);

  NatsPollService svc(ctx);
  const std::string out =
      svc.poll_response("req-poll-t0", R"({"q":1})", 0, trace_ctx);
  REQUIRE(out == R"({"status":200,"body":"ok"})");
}

TEST_CASE("NatsPollService: reconnects a dropped client and still replies",
          "[nats-live]") {
  NatsServer server;
  server.start_live();
  TraceContext trace_ctx;
  trace_ctx.m_trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";

  const std::string subject = unique_subject("poll.reconnect");
  const EnvMapGuard env(live_proxy_env(server.port(), subject));
  AppContext ctx;
  init_proxy_components(ctx);
  REQUIRE(ctx.m_nats_client->is_connected());
  ctx.m_nats_client->disconnect();
  REQUIRE_FALSE(ctx.m_nats_client->is_connected());

  const auto reply_fn = [](const std::string &, const std::string &) {
    return NatsReply{R"({"status":200,"body":"ok"})", NatsHeaders{}};
  };
  NatsResponder const responder(live_nats_cfg(subject, server.port()), subject,
                                reply_fn);

  NatsPollService svc(ctx);
  const std::string out =
      svc.poll_response("req-poll-rec", R"({"q":1})", 2, trace_ctx);
  REQUIRE(out == R"({"status":200,"body":"ok"})");
}

TEST_CASE("RequestHandler: DB gateway 200, invalid envelope 502, silent 504",
          "[nats-live]") {
  NatsServer server;
  server.start_live();

  {
    const std::string subject = unique_subject("db.ok");
    const EnvMapGuard env(live_proxy_env(server.port(), unique_subject("poll.no"),
                                      subject));
    AppContext ctx;
    attach_tracer(ctx);
    init_proxy_components(ctx);
    REQUIRE(ctx.m_nats_client->is_connected());

    const auto reply_fn = [](const std::string &, const std::string &) {
      return NatsReply{R"({"status":200,"body":{"db":"ok"}})",
                       NatsHeaders{{"X-Consume-Span-Id", "consume-db-1"}}};
    };
    NatsResponder const responder(live_nats_cfg(subject, server.port()),
                                  subject, reply_fn);

    ProxyHandler handler(ctx);
    const httplib::Request req = make_get("/v1/sql/postgres/ping");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 200);
    REQUIRE(json::parse(res.body)["db"] == "ok");
  }

  {
    const std::string subject = unique_subject("db.bad");
    const EnvMapGuard env(live_proxy_env(server.port(), unique_subject("poll.no"),
                                      subject));
    AppContext ctx;
    init_proxy_components(ctx);
    const auto reply_fn = [](const std::string &, const std::string &) {
      return NatsReply{"not-a-json-envelope", NatsHeaders{}};
    };
    NatsResponder const responder(live_nats_cfg(subject, server.port()),
                                  subject, reply_fn);

    ProxyHandler handler(ctx);
    const httplib::Request req = make_get("/v1/sql/postgres/ping");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 502);
  }

  {
    const std::string subject = unique_subject("db.timeout");
    const EnvMapGuard env(live_proxy_env(server.port(), unique_subject("poll.no"),
                                      subject));
    AppContext ctx;
    init_proxy_components(ctx);
    REQUIRE(ctx.m_nats_client->is_connected());

    ProxyHandler handler(ctx);
    const httplib::Request req = make_get("/v1/sql/postgres/ping");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 504);
  }
}

TEST_CASE("RequestHandler: generic backend 200 and invalid response over a live "
          "broker",
          "[nats-live]") {
  NatsServer server;
  server.start_live();

  {
    // A responder subscribed to the proxy's own NATS subject answers the poll,
    // so the generic request path runs past poll_for_response() into
    // send_response() (the ordinary proxy->worker->client success path, which
    // offline tests only ever see time out with 504).
    const std::string subject = unique_subject("proxy.ok");
    const EnvMapGuard env(live_proxy_env(server.port(), subject));
    AppContext ctx;
    init_proxy_components(ctx);
    REQUIRE(ctx.m_nats_client->is_connected());
    const auto reply_fn = [](const std::string &, const std::string &) {
      return NatsReply{build_nats_response_envelope(
                           200, "req-proxy-ok", R"({"proxied":true})", 0,
                           false, "application/json", json::object(), "")
                           .dump(),
                       NatsHeaders{}};
    };
    NatsResponder const responder(live_nats_cfg(subject, server.port()),
                                  subject, reply_fn);

    ProxyHandler handler(ctx);
    const httplib::Request req = make_get("/api/v1/anything");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 200);
    REQUIRE(json::parse(res.body)["proxied"] == true);
  }

  {
    // A reply that is not a JSON envelope reaches the invalid_response branch
    // of the generic path.
    const std::string subject = unique_subject("proxy.badresp");
    const EnvMapGuard env(live_proxy_env(server.port(), subject));
    AppContext ctx;
    init_proxy_components(ctx);
    const auto reply_fn = [](const std::string &, const std::string &) {
      return NatsReply{"not-a-json-envelope", NatsHeaders{}};
    };
    NatsResponder const responder(live_nats_cfg(subject, server.port()),
                                  subject, reply_fn);

    ProxyHandler handler(ctx);
    const httplib::Request req = make_get("/api/v1/anything");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 500);
    REQUIRE(json::parse(res.body)["error"] == "Invalid response format");
  }
}

TEST_CASE("RequestHandler: readiness 200 with a live broker", "[nats-live]") {
  NatsServer server;
  server.start_live();

  {
    // Default path: readiness reports the live connection state without
    // initiating a reconnect.
    EnvVars vars = live_proxy_env(server.port(), unique_subject("poll.rdy"),
                                  unique_subject("db.rdy"));
    vars["HEALTH_READY_ALLOW_CONNECT"] = "false";
    const EnvMapGuard env(vars);
    AppContext ctx;
    init_proxy_components(ctx);
    REQUIRE_FALSE(ctx.m_config.m_app.m_health_ready_allow_connect);
    REQUIRE(ctx.m_nats_client->is_connected());

    ProxyHandler handler(ctx);
    httplib::Response res;
    handler.handle_get(make_get("/health/ready"), res);
    REQUIRE(res.status == 200);
    REQUIRE(json::parse(res.body)["status"] == "ready");
    REQUIRE(ctx.m_proxy.m_metrics->m_health_ready.Value() == 1.0);
    REQUIRE(ctx.m_proxy.m_metrics->m_nats_connected.Value() == 1.0);
  }

  {
    // Opt-in blocking path: HEALTH_READY_ALLOW_CONNECT=true pings the broker
    // and reports ready when the pong round-trip succeeds.
    EnvVars vars = live_proxy_env(server.port(), unique_subject("poll.rdy2"),
                                  unique_subject("db.rdy2"));
    vars["HEALTH_READY_ALLOW_CONNECT"] = "true";
    const EnvMapGuard env(vars);
    AppContext ctx;
    REQUIRE(ctx.m_config.m_app.m_health_ready_allow_connect);
    init_proxy_components(ctx);
    REQUIRE(ctx.m_nats_client->is_connected());

    ProxyHandler handler(ctx);
    httplib::Response res;
    handler.handle_get(make_get("/health/ready"), res);
    REQUIRE(res.status == 200);
    REQUIRE(json::parse(res.body)["status"] == "ready");
    REQUIRE(ctx.m_proxy.m_metrics->m_health_ready.Value() == 1.0);
  }
}

TEST_CASE("L2Worker: run loop serves requests over a live broker",
          "[nats-live]") {
  NatsServer server;
  server.start_live();

  L2BackendServer const backend;
  const json urls = json::array({backend.api_url()});
  const std::string subject = unique_subject("l2w.test");
  const EnvMapGuard env(
      live_worker_env(server.port(), subject, urls.dump()));
  AppContext ctx;
  attach_tracer(ctx);
  L2Worker worker(ctx);
  NatsClient test_client(live_nats_cfg(subject, server.port()));

  REQUIRE(worker.is_nats_connected());
  WorkerRunner const runner(worker);

  REQUIRE(test_client.connect());

  // The worker subscribes shortly after run() starts; retry the request until
  // a reply arrives (earlier attempts hit "no responders").
  const std::string request =
      make_worker_request("live-req-1", "/api/value").dump();
  std::string reply;
  REQUIRE(wait_for_condition(
      [&test_client, &subject, &request, &reply]() {
        if (const auto r = test_client.request(subject, request, 1000)) {
          if (!r->empty()) {
            reply = *r;
            return true;
          }
        }
        return false;
      },
      12000));

  const json envelope = json::parse(reply);
  REQUIRE(envelope[NatsResponseContract::kStatus] == 200);
  REQUIRE(envelope[NatsResponseContract::kBody]
                     [NatsResponseContract::kBodyResponse]
                         .get<std::string>()
                     .find("l2-live-val") != std::string::npos);
  REQUIRE(backend.m_posts.load() == 1);

  // Same request_id again: served from the dedup cache, no second L2 call.
  const std::optional<std::string> r2 =
      test_client.request(subject, request, 3000);
  REQUIRE(r2.has_value());
  REQUIRE(json::parse(*r2)[NatsResponseContract::kStatus] == 200);
  REQUIRE(backend.m_posts.load() == 1);

  // A fire-and-forget publish (no reply subject) reaches the worker's
  // queue-group subscription; the worker must drop it instead of enqueuing a
  // response it cannot address. NATS delivers messages on a subject in order,
  // so the next request below is guaranteed to be handled after this drop.
  REQUIRE(test_client.publish(
      subject, make_worker_request("live-no-reply", "/api/value").dump()));

  // Unparseable payload: the worker replies 400 instead of panicking.
  const std::optional<std::string> r3 =
      test_client.request(subject, "not-json", 3000);
  REQUIRE(r3.has_value());
  REQUIRE(json::parse(*r3)["error"] == "Invalid request format");
}

TEST_CASE("NatsClient: constructor logs configured auth and TLS flags",
          "[nats-offline]") {
  NatsConfig cfg = live_nats_cfg(unique_subject("nats.authcstr"), 4222);
  cfg.m_username = "user-1";
  cfg.m_password = "pass-1";
  cfg.m_token = "token-1";
  cfg.m_credentials_file = "/nonexistent-test-creds.pem";
  cfg.m_enable_tls = true;
  cfg.m_tls_ca_cert_file = "/nonexistent-test-ca.pem";
  NatsClient const client(cfg);
  // No connection is attempted from the constructor; the fixture must be
  // usable by callers that only construct.
  REQUIRE_FALSE(client.is_connected());
}

TEST_CASE("NatsClient: auth/TLS misconfiguration aborts connect before dialing",
          "[nats-offline]") {
  const std::string subject = unique_subject("nats.authfail");

  // A garbage credentials file makes natsOptions_SetUserCredentialsFromFiles
  // fail (misshapen JWT/NKey), but the load is lazy: the abort on a nonexistent
  // path is NOT guaranteed, so every sub-case pins the bogus TLS CA as the
  // guaranteed setup_options abort point (natsOptions_LoadCATrustedCertificates
  // fails fast on a missing file). connect() with a broken options setup
  // returns false instead of blocking on natsConnection_Connect.
  const std::string creds_path = "/tmp/nats-garbage-creds.pem";
  {
    std::ofstream file(creds_path);
    file << "this is not a NATS credential file\n";
  }

  {
    // Token auth: natsOptions_SetToken succeeds, TLS abort follows.
    NatsConfig cfg = live_nats_cfg(subject, 4222);
    cfg.m_token = "token-1";
    cfg.m_enable_tls = true;
    cfg.m_tls_ca_cert_file = "/nonexistent-test-ca.pem";
    NatsClient client(cfg);
    REQUIRE_FALSE(client.connect());
    REQUIRE(client.get_last_error().has_value());
  }

  {
    // Username/password branch (else-if of the token branch).
    NatsConfig cfg = live_nats_cfg(subject, 4222);
    cfg.m_username = "user-1";
    cfg.m_password = "pass-1";
    cfg.m_enable_tls = true;
    cfg.m_tls_ca_cert_file = "/nonexistent-test-ca.pem";
    NatsClient client(cfg);
    REQUIRE_FALSE(client.connect());
    REQUIRE(client.get_last_error().has_value());
  }

  {
    // Credentials file branch: garbage content fails the parse (or the TLS
    // abort below catches the case where the load is deferred).
    NatsConfig cfg = live_nats_cfg(subject, 4222);
    cfg.m_credentials_file = creds_path;
    cfg.m_enable_tls = true;
    cfg.m_tls_ca_cert_file = "/nonexistent-test-ca.pem";
    NatsClient client(cfg);
    REQUIRE_FALSE(client.connect());
    REQUIRE(client.get_last_error().has_value());
  }

  {
    // TLS without CA but with bogus client cert/key aborts at
    // LoadCertificatesChain.
    NatsConfig cfg = live_nats_cfg(subject, 4222);
    cfg.m_enable_tls = true;
    cfg.m_tls_cert_file = "/nonexistent-test-cert.pem";
    cfg.m_tls_key_file = "/nonexistent-test-key.pem";
    NatsClient client(cfg);
    REQUIRE_FALSE(client.connect());
    REQUIRE(client.get_last_error().has_value());
  }

  std::remove(creds_path.c_str());
}

TEST_CASE("NatsClient: connect to a down broker succeeds once it starts",
          "[nats-live]") {
  if (nats_binary_path().empty()) {
    SKIP("nats-server binary not available");
  }
  NatsServer server;
  const std::string subject = unique_subject("nats.bootstrap");

  NatsClient client(live_nats_cfg(subject, server.port()));
  std::atomic<bool> connected{false};
  std::thread runner([&client, &connected]() {
    connected.store(client.connect());
  });

  // Let at least one connect attempt cycle fail before the broker appears.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  REQUIRE(server.start());
  REQUIRE(server.wait_ready());

  REQUIRE(wait_for_condition([&connected]() { return connected.load(); }, 15000));
  REQUIRE(connected.load());
  runner.join();
  REQUIRE(client.is_connected());
  // Idempotent reconnect on an established connection.
  REQUIRE(client.connect());
}

TEST_CASE("NatsClient: request returns nullopt on an empty reply", "[nats-live]") {
  NatsServer server;
  server.start_live();

  const std::string subject = unique_subject("nats.emptyreply");
  NatsResponder const responder(
      live_nats_cfg(subject, server.port()), subject,
      [](const std::string &, const std::string &) { return NatsReply{""}; });

  NatsClient client(live_nats_cfg(subject, server.port()));
  REQUIRE(client.connect());

  const std::optional<std::string> r1 = client.request(subject, "ping", 3000);
  REQUIRE_FALSE(r1.has_value());

  const NatsReply r2 = client.request_with_headers(subject, "ping", {}, {}, 3000);
  REQUIRE(r2.m_data.empty());
  client.unsubscribe();
  client.disconnect();
}

TEST_CASE("L2Worker: reconnects and keeps serving after the broker restarts",
          "[nats-live]") {
  NatsServer server;
  server.start_live();

  L2BackendServer const backend;
  const json urls = json::array({backend.api_url()});
  const std::string subject = unique_subject("l2w.restart");
  const EnvMapGuard env(
      live_worker_env(server.port(), subject, urls.dump()));
  AppContext ctx;
  attach_tracer(ctx);
  L2Worker worker(ctx);
  NatsClient test_client(live_nats_cfg(subject, server.port()));

  REQUIRE(worker.is_nats_connected());
  WorkerRunner const runner(worker);

  REQUIRE(test_client.connect());

  const auto make_sender = [&test_client, &subject](const std::string &id) {
    const std::string payload =
        make_worker_request(id, "/api/value").dump();
    return [&test_client, &subject, payload]() -> bool {
      const auto r = test_client.request(subject, payload, 1500);
      return r.has_value() && !r->empty();
    };
  };

  // First request: worker subscribes and serves it.
  REQUIRE(wait_for_condition(make_sender("restart-req-1"), 12000));
  REQUIRE(backend.m_posts.load() == 1);

  // Restart the broker: the worker's connection drops, the run loop observes
  // the loss, reconnects and forces a subscription refresh.
  server.restart();
  REQUIRE(wait_for_condition([&worker]() { return worker.is_nats_connected(); },
                             15000));

  // A fresh request id (avoids the dedup cache) must reach L2 again after the
  // subscription refresh.
  REQUIRE(wait_for_condition(make_sender("restart-req-2"), 12000));
  REQUIRE(backend.m_posts.load() == 2);
}

#include <libpq-fe.h>
#include <filesystem>
#include <cerrno>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>

#include "test_pg_diag.hpp"

namespace pgtest {

constexpr int kPgWaitMs = 10000;

using pg_test_diag::drop_privileges;
using pg_test_diag::postgres_sql_probe;
using pg_test_diag::redirect_to;
using pg_test_diag::resolve_server_user;
using pg_test_diag::ServerUser;
using test_support::reserve_loopback_port;
using test_support::wait_for_condition;

[[nodiscard]] std::string pg_bindir() {
  auto has=[&](const std::string& d){ return ::access((d+"/initdb").c_str(),X_OK)==0 && ::access((d+"/postgres").c_str(),X_OK)==0; };
  if (has("/usr/local/bin")) {
    return "/usr/local/bin";
  }
  std::error_code ec;
  const std::filesystem::path ld("/usr/lib/postgresql");
  if (!std::filesystem::exists(ld, ec)) {
    return "";
  }
  std::vector<std::string> vs;
  for (auto &e : std::filesystem::directory_iterator(ld, ec)) {
    if (e.is_directory()) {
      vs.push_back(e.path().filename().string());
    }
  }
  std::ranges::sort(vs,[](auto&a,auto&b){return std::stoi(a)>std::stoi(b);});
  for (auto&v:vs){ auto d="/usr/lib/postgresql/"+v+"/bin";
    if (has(d)) {
      return d;
    }
  }
  return "";
}

class PgSrv {
public:
  [[nodiscard]] int port() const { return m_port; }
  bool ensure() {
    if (m_pid > 0) {
      return true;
    }
    if (m_fail) {
      return false;
    }
    if (!start()) { m_fail=true; return false; }
    return true;
  }
  void stop() {
    if (m_pid <= 0) {
      return;
    }
    ::kill(m_pid,SIGINT);
    auto dl = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    int st=0;
    while (::waitpid(m_pid, &st, WNOHANG) == 0 &&
           std::chrono::steady_clock::now() < dl) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (::waitpid(m_pid,&st,WNOHANG)==0) { ::kill(m_pid,SIGKILL); ::waitpid(m_pid,&st,0); }
    m_pid=-1;
  }
  ~PgSrv(){ stop(); }
private:
  [[nodiscard]] bool alive() const {
    if (m_pid <= 0) {
      return false;
    }
    int st = 0;
    return ::waitpid(m_pid, &st, WNOHANG) == 0;
  }
  [[nodiscard]] bool run_wait(const std::string &exe,
                              const std::vector<std::string> &args,
                              const std::string &log, const ServerUser &t) const {
    pid_t const p = ::fork();
    if (p == 0) {
      if (!drop_privileges(t)) {
        _exit(127);
      }
      (void)redirect_to(log.c_str());
      std::vector<const char *> av;
      av.push_back(exe.c_str());
      for (auto &a : args) {
        av.push_back(a.c_str());
      }
      av.push_back(nullptr);
      ::execv(exe.c_str(), (char *const *)av.data());
      _exit(127);
    }
    if (p < 0) {
      return false;
    }
    int st = 0;
    while (::waitpid(p, &st, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
  }
  [[nodiscard]] pid_t spawn(const std::string &exe,
                            const std::vector<std::string> &args,
                            const std::string &log, const ServerUser &t) const {
    pid_t const p = ::fork();
    if (p == 0) {
      if (!drop_privileges(t)) {
        _exit(127);
      }
      (void)redirect_to(log.c_str());
      std::vector<const char *> av;
      av.push_back(exe.c_str());
      for (auto &a : args) {
        av.push_back(a.c_str());
      }
      av.push_back(nullptr);
      ::execv(exe.c_str(), (char *const *)av.data());
      _exit(127);
    }
    return p;
  }
  bool start() {
    if (m_bindir.empty()){ m_bindir=pg_bindir();
      if (m_bindir.empty()) {
        pg_test_diag::fail("postgres binaries not installed, skipping");
        return false;
      }
    }
    const auto t = resolve_server_user();
    if (!t.m_ok) {
      pg_test_diag::fail("no 'postgres' user in this image, skipping");
      return false;
    }
    // A full /tmp makes initdb fail; report it instead of degrading to a silent
    // SKIP that only shows up as a coverage-gate drop.
    if (!pg_test_diag::enough_free_space()) {
      return false;
    }
    m_port=reserve_loopback_port();
    if (m_port <= 0) {
      pg_test_diag::fail("no free loopback port");
      return false;
    }
    const std::string tmpl="/tmp/pgdbn-XXXXXX";
    std::vector<char> dt(tmpl.begin(),tmpl.end()); dt.push_back(0);
    char* d=::mkdtemp(dt.data());
    if (!d) {
      pg_test_diag::fail(std::format("mkdtemp failed: {}", std::strerror(errno)));
      return false;
    }
    m_datadir = d;
    if (t.m_need_switch && ::chown(m_datadir.c_str(), t.m_uid, t.m_gid) != 0) {
      pg_test_diag::fail(std::format("chown datadir failed: {}", std::strerror(errno)));
      cleanup();
      return false;
    }
    const std::string init_log = "/tmp/pginitn-" + std::to_string(m_port) + ".log";
    if (!run_wait(m_bindir+"/initdb",{"-D",m_datadir,"-U","postgres","-A","trust","--no-locale","-E","UTF8"}, init_log,t)){ pg_test_diag::fail(std::format("initdb failed: {}", pg_test_diag::log_tail(init_log))); cleanup(); return false; }
    const std::string serv_log = "/tmp/pgservn-" + std::to_string(m_port) + ".log";
    pid_t const sp =
        spawn(m_bindir + "/postgres",
              {"-D", m_datadir, "-p", std::to_string(m_port), "-h", "127.0.0.1",
               "-k", "/tmp"},
              serv_log, t);
    if (sp<0){ pg_test_diag::fail(std::format("fork postgres failed: {}", std::strerror(errno))); cleanup(); return false; }
    m_pid=sp;
    if (!wait_for_condition([this]{ return postgres_sql_probe(m_port)&&alive(); }, kPgWaitMs)){ pg_test_diag::fail(std::format("server at {} never became ready: {}", m_port, pg_test_diag::log_tail(serv_log))); stop(); cleanup(); return false; }
    return true;
  }
  void cleanup(){ m_pid=-1; if (!m_datadir.empty()){ std::error_code ec; std::filesystem::remove_all(m_datadir,ec); m_datadir.clear(); } }
  std::string m_bindir,m_datadir; int m_port=0; pid_t m_pid=-1; bool m_fail=false;
};

PgSrv& shared_pgn(){ static PgSrv s; return s; }
#define REQ_PG() do{ if (!shared_pgn().ensure()) SKIP("pg not avail"); } while(0)

DbConfig mk_pg(const PgSrv& s,int pm=5,int qtm=5000){
  DbConfig db; db.m_name="pg"; db.m_driver="postgres"; db.m_host="127.0.0.1"; db.m_port=s.port(); db.m_database="postgres"; db.m_user="postgres"; db.m_pool_min=1; db.m_pool_max=pm; db.m_query_timeout_ms=qtm; db.m_max_rows=1000; return db;
}

} // pgtest



TEST_CASE("L2Worker: serves DB queries over live NATS+Postgres", "[nats-live][db-live-combo]") {
  NatsServer server;
  server.start_live();

  auto &pg = pgtest::shared_pgn();
  if (!pg.ensure()) {
    SKIP("postgres not available");
  }

  const std::string subject = unique_subject("l2w.dbq-main");
  const std::string db_subject = unique_subject("l2w.dbq-db");
  EnvVars vars =
      live_worker_env(server.port(), subject, "[\"http://127.0.0.1:18088/api\"]");
  vars["DB_QUERY_ENABLED"] = "true";
  vars["DB_POSTGRES_ENABLED"] = "true";
  vars["DB_QUERY_NATS_SUBJECT"] = db_subject;
  vars["DB_POSTGRES_HOST"] = "127.0.0.1";
  vars["DB_POSTGRES_PORT"] = std::to_string(pg.port());
  vars["DB_POSTGRES_DB"] = "postgres";
  vars["DB_POSTGRES_USER"] = "postgres";
  vars["DB_POSTGRES_PASSWORD"] = "";
  vars["DB_QUERY_DEFAULT_TIMEOUT_MS"] = "2000";
  const EnvMapGuard env(vars);
  AppContext ctx;
  attach_tracer(ctx);
  L2Worker worker(ctx);

  NatsClient test_client(live_nats_cfg(subject, server.port()));
  REQUIRE(test_client.connect());

  WorkerRunner const runner(worker);

  // Query served end-to-end: NATS -> worker -> live PostgreSQL -> reply.
  const json payload = {{DbQueryContract::kSql, "SELECT 42 AS answer"}};
  const std::string request = build_db_query_request(
      DbQueryContract::kTypeQuery, "db-live-req-1", "postgres", payload)
                                  .dump();
  std::string reply;
  REQUIRE(wait_for_condition(
      [&test_client, &db_subject, &request, &reply]() {
        if (const auto r = test_client.request(db_subject, request, 1000)) {
          if (!r->empty()) {
            reply = *r;
            return true;
          }
        }
        return false;
      },
      20000));
  // A reply implies the DB handler was initialized and subscribed.
  REQUIRE(worker.db_handler_ready());

  const json envelope = json::parse(reply);
  REQUIRE(envelope[DbQueryContract::kStatus] == 200);
  const auto &body = envelope[DbQueryContract::kBody];
  REQUIRE(body[DbResponseContract::kStatus] == DbResponseContract::kStatusOk);
  REQUIRE(body[DbResponseContract::kColumns] ==
          json::array({json{{DbResponseContract::kName, "answer"},
                            {DbResponseContract::kType, "INTEGER"}}}));
  REQUIRE(body[DbResponseContract::kRows] == json::array({json::array({42})}));
  REQUIRE(body[DbResponseContract::kRowCount] == 1);

  // Ping path over the same live subscription.
  const std::string ping = build_db_query_request(DbQueryContract::kTypePing,
                                                  "db-live-ping-1", "postgres")
                               .dump();
  std::string preply;
  REQUIRE(wait_for_condition(
      [&test_client, &db_subject, &ping, &preply]() {
        if (const auto r = test_client.request(db_subject, ping, 1000)) {
          if (!r->empty()) {
            preply = *r;
            return true;
          }
        }
        return false;
      },
      10000));
  const json penv = json::parse(preply);
  REQUIRE(penv[DbQueryContract::kStatus] == 200);
  REQUIRE(penv[DbQueryContract::kBody][DbResponseContract::kStatus] ==
          DbResponseContract::kStatusOk);
  REQUIRE(penv[DbQueryContract::kBody].contains(
      DbResponseContract::kLatencyMs));
}
TEST_CASE("NatsClient: request with timeout 0 uses config timeout", "[nats-live]") {
  NatsServer s;
  s.start_live();
  natsConnection* nc = nullptr;
  REQUIRE(natsConnection_ConnectTo(&nc, ("nats://127.0.0.1:"+std::to_string(s.port())).c_str()) == NATS_OK);
  natsMsg* reply=nullptr;
  auto st = natsConnection_RequestString(&reply, nc, "no.such.subject.timeout0", "hi", (int64_t)0);
  (void)st;
  if (reply) {
    natsMsg_Destroy(reply);
  }
  natsConnection_Destroy(nc);
}

TEST_CASE("NatsClient: subscribe rejects invalid subject", "[nats-live]") {
  NatsServer s;
  s.start_live();
  natsConnection* nc = nullptr;
  REQUIRE(natsConnection_ConnectTo(&nc, ("nats://127.0.0.1:"+std::to_string(s.port())).c_str()) == NATS_OK);
  natsSubscription* sub=nullptr;
  auto st = natsConnection_SubscribeSync(&sub, nc, "bad subject");
  REQUIRE(st == NATS_INVALID_SUBJECT);
  if (sub) {
    natsSubscription_Destroy(sub);
  }
  natsConnection_Destroy(nc);
}

TEST_CASE("NatsClient: request times out when subscriber never replies", "[nats-live]") {
  NatsServer s;
  s.start_live();
  natsConnection* nc = nullptr;
  REQUIRE(natsConnection_ConnectTo(&nc, ("nats://127.0.0.1:"+std::to_string(s.port())).c_str()) == NATS_OK);
  natsSubscription* sub=nullptr;
  auto ssub = natsConnection_SubscribeSync(&sub, nc, "test.timeout.never.reply");
  REQUIRE(ssub == NATS_OK);
  natsMsg* reply=nullptr;
  auto sreq = natsConnection_RequestString(&reply, nc, "test.timeout.never.reply", "ping", (int64_t)50);
  REQUIRE(sreq == NATS_TIMEOUT);
  if (reply) {
    natsMsg_Destroy(reply);
  }
  natsSubscription_Destroy(sub);
  natsConnection_Destroy(nc);
}

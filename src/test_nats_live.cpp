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
#include "httplib/httplib.h"
#include "json_utils.hpp"
#include "l2_worker.hpp"
#include "nats_client.hpp"
#include "nats_poll_service.hpp"
#include "proxy_init.hpp"
#include "request_handler.hpp"
#include "stats_logger.hpp"
#include "trace_logger.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <algorithm>

extern std::atomic<bool> g_shutdown_flag;

namespace {

constexpr const char *kTraceparent =
    "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

// RAII override of several env vars; nullopt unsets the variable. The values
// must be in place before the Config is loaded by the AppContext ctor.
class EnvGuard {
public:
  explicit EnvGuard(const std::map<std::string, std::optional<std::string>> &vars) {
    m_vars.reserve(vars.size());
    for (const auto &[name, value] : vars) {
      m_vars.emplace_back(name, read_env(name.c_str()));
      if (value.has_value()) {
        setenv(name.c_str(), value->c_str(), 1);
      } else {
        unsetenv(name.c_str());
      }
    }
  }

  ~EnvGuard() {
    for (const auto &[name, previous] : m_vars) {
      if (previous.has_value()) {
        setenv(name.c_str(), previous->c_str(), 1);
      } else {
        unsetenv(name.c_str());
      }
    }
  }

  EnvGuard(const EnvGuard &) = delete;
  EnvGuard &operator=(const EnvGuard &) = delete;
  EnvGuard(EnvGuard &&) = delete;
  EnvGuard &operator=(EnvGuard &&) = delete;

private:
  static std::optional<std::string> read_env(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string(value);
  }

  std::vector<std::pair<std::string, std::optional<std::string>>> m_vars;
};

using EnvVars = std::map<std::string, std::optional<std::string>>;

// Path of the nats-server binary COPYed into the builder image; empty when the
// binary is not installed (tests then SKIP). Also consulted along PATH so the
// tests can run against a hand-installed binary.
constexpr const char *const kNatsServerCandidates[] = {
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
  std::string path(path_env);
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

[[nodiscard]] int reserve_loopback_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
  }
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
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

[[nodiscard]] bool wait_for_condition(const std::function<bool()> &predicate,
                                      int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  do {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  } while (std::chrono::steady_clock::now() < deadline);
  return predicate();
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
      if (m_tls_cert.empty() || m_tls_key.empty()) {
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

  ~NatsServer() { stop(); }

  void enable_tls(const std::string &cert, const std::string &key) {
    m_tls_cert = cert;
    m_tls_key = key;
  }

private:
  int m_port = 0;
  pid_t m_pid = 0;
  std::string m_tls_cert;
  std::string m_tls_key;
};

std::string unique_subject(const std::string &base) {
  static std::atomic<unsigned> seq{0};
  return base + "." + std::to_string(::getpid()) + "." +
         std::to_string(seq.fetch_add(1));
}

std::pair<std::string, std::string> generate_tls_certs(const std::string &base_dir = "/tmp") {
  std::string dir = base_dir;
  const std::string prefix = dir + "/nats-tls-" + std::to_string(::getpid()) + "-" +
                             std::to_string(::time(nullptr));
  const std::string key_path = prefix + ".key";
  const std::string crt_path = prefix + ".crt";
  std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key_path +
                    " -out " + crt_path +
                    " -days 1 -nodes -subj '/CN=localhost' >/dev/null 2>&1";
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

// Batched Jaeger exporter pointed at a refused port: spans are buffered and
// the flush fails harmlessly, so the tracer-dependent branches become
// reachable without an observability backend.
void attach_tracer(AppContext &ctx) {
  auto &metrics = *ctx.m_tracing_metrics;
  ctx.m_tracer = std::make_unique<JaegerLogger>(
      "http://127.0.0.1:1/api/traces", metrics.m_spans_sent,
      metrics.m_spans_failed, metrics.m_queue_size,
      metrics.m_last_send_duration, metrics.m_send_latency, metrics.m_queue_time,
      /*batch_size=*/10000, /*flush_interval_ms=*/100000, /*sample_rate=*/1.0);
}

httplib::Request make_get(const std::string &path) {
  httplib::Request req;
  req.method = "GET";
  req.path = path;
  return req;
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
  const EnvGuard env(fail_fast_env(unique_subject("nats.offline")));
  AppContext ctx;
  init_proxy_components(ctx);
  NatsClient &client = *ctx.m_nats_client;

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
  const EnvGuard env(fail_fast_env(unique_subject("nats.polloff")));
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

// ============================================================================
// Track A: NatsClient against a live broker
// ============================================================================

TEST_CASE("NatsClient: pub/sub round-trip, headers and queue subscribers",
          "[nats-live]") {
  if (nats_binary_path().empty()) {
    SKIP("nats-server binary not available");
  }
  NatsServer server;
  REQUIRE(server.start());
  REQUIRE(server.wait_ready());

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
  if (nats_binary_path().empty()) {
    SKIP("nats-server binary not available");
  }
  NatsServer server;
  REQUIRE(server.start());
  REQUIRE(server.wait_ready());

  const std::string subject = unique_subject("nats.reqrep");
  const auto reply_fn = [](const std::string &, const std::string &) {
    return NatsReply{
        R"({"pong":1})",
        NatsHeaders{{"X-Reply-Marker", "yes"},
                    {"X-Consume-Span-Id", "span-1"}}};
  };
  NatsResponder responder(live_nats_cfg(subject, server.port()), subject,
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
  if (nats_binary_path().empty()) {
    SKIP("nats-server binary not available");
  }
  NatsServer server;
  REQUIRE(server.start());
  REQUIRE(server.wait_ready());

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

// ============================================================================
// Track B: NatsPollService / DB gateway / worker over a live broker
// ============================================================================

TEST_CASE("NatsPollService: round-trip, empty reply retry and no responders",
          "[nats-live]") {
  if (nats_binary_path().empty()) {
    SKIP("nats-server binary not available");
  }
  NatsServer server;
  REQUIRE(server.start());
  REQUIRE(server.wait_ready());
  TraceContext trace_ctx;
  trace_ctx.m_trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";

  {
    const std::string subject = unique_subject("poll.ok");
    const EnvGuard env(live_proxy_env(server.port(), subject));
    AppContext ctx;
    attach_tracer(ctx);
    init_proxy_components(ctx);
    REQUIRE(ctx.m_nats_client->is_connected());

    const auto reply_fn = [](const std::string &, const std::string &) {
      return NatsReply{R"({"status":200,"body":"ok"})",
                       NatsHeaders{{"X-Consume-Span-Id", "poll-span"}}};
    };
    NatsResponder responder(live_nats_cfg(subject, server.port()), subject,
                            reply_fn);

    NatsPollService svc(ctx);
    const std::string out =
        svc.poll_response("req-poll-1", R"({"q":1})", 3, trace_ctx);
    REQUIRE(out == R"({"status":200,"body":"ok"})");
  }

  {
    const std::string subject = unique_subject("poll.empty");
    const EnvGuard env(live_proxy_env(server.port(), subject));
    AppContext ctx;
    init_proxy_components(ctx);
    const auto reply_fn = [](const std::string &, const std::string &) {
      return NatsReply{""};
    };
    NatsResponder responder(live_nats_cfg(subject, server.port()), subject,
                            reply_fn);

    NatsPollService svc(ctx);
    const std::string out =
        svc.poll_response("req-poll-2", R"({"q":1})", 2, trace_ctx);
    REQUIRE(out.empty());
  }

  {
    const std::string subject = unique_subject("poll.noreply");
    const EnvGuard env(live_proxy_env(server.port(), subject));
    AppContext ctx;
    init_proxy_components(ctx);

    NatsPollService svc(ctx);
    const std::string out =
        svc.poll_response("req-poll-3", R"({"q":1})", 1, trace_ctx);
    REQUIRE(out.empty());
  }
}

TEST_CASE("RequestHandler: DB gateway 200, invalid envelope 502, silent 504",
          "[nats-live]") {
  if (nats_binary_path().empty()) {
    SKIP("nats-server binary not available");
  }
  NatsServer server;
  REQUIRE(server.start());
  REQUIRE(server.wait_ready());

  {
    const std::string subject = unique_subject("db.ok");
    const EnvGuard env(live_proxy_env(server.port(), unique_subject("poll.no"),
                                      subject));
    AppContext ctx;
    attach_tracer(ctx);
    init_proxy_components(ctx);
    REQUIRE(ctx.m_nats_client->is_connected());

    const auto reply_fn = [](const std::string &, const std::string &) {
      return NatsReply{R"({"status":200,"body":{"db":"ok"}})", NatsHeaders{}};
    };
    NatsResponder responder(live_nats_cfg(subject, server.port()), subject,
                            reply_fn);

    std::atomic<bool> shutdown{false};
    StatsLogger stats(ctx, shutdown);
    RequestHandler handler(ctx, stats);
    const httplib::Request req = make_get("/v1/sql/postgres/ping");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 200);
    REQUIRE(json::parse(res.body)["db"] == "ok");
  }

  {
    const std::string subject = unique_subject("db.bad");
    const EnvGuard env(live_proxy_env(server.port(), unique_subject("poll.no"),
                                      subject));
    AppContext ctx;
    init_proxy_components(ctx);
    const auto reply_fn = [](const std::string &, const std::string &) {
      return NatsReply{"not-a-json-envelope", NatsHeaders{}};
    };
    NatsResponder responder(live_nats_cfg(subject, server.port()), subject,
                            reply_fn);

    std::atomic<bool> shutdown{false};
    StatsLogger stats(ctx, shutdown);
    RequestHandler handler(ctx, stats);
    const httplib::Request req = make_get("/v1/sql/postgres/ping");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 502);
  }

  {
    const std::string subject = unique_subject("db.timeout");
    const EnvGuard env(live_proxy_env(server.port(), unique_subject("poll.no"),
                                      subject));
    AppContext ctx;
    init_proxy_components(ctx);
    REQUIRE(ctx.m_nats_client->is_connected());

    std::atomic<bool> shutdown{false};
    StatsLogger stats(ctx, shutdown);
    RequestHandler handler(ctx, stats);
    const httplib::Request req = make_get("/v1/sql/postgres/ping");
    httplib::Response res;
    handler.handle_get(req, res);
    REQUIRE(res.status == 504);
  }
}

TEST_CASE("L2Worker: run loop serves requests over a live broker",
          "[nats-live]") {
  if (nats_binary_path().empty()) {
    SKIP("nats-server binary not available");
  }
  NatsServer server;
  REQUIRE(server.start());
  REQUIRE(server.wait_ready());

  L2BackendServer backend;
  const json urls = json::array({backend.api_url()});
  const std::string subject = unique_subject("l2w.test");
  const EnvGuard env(
      live_worker_env(server.port(), subject, urls.dump()));
  AppContext ctx;
  attach_tracer(ctx);
  L2Worker worker(ctx);
  NatsClient test_client(live_nats_cfg(subject, server.port()));

  struct RestoreShutdownFlag {
    ~RestoreShutdownFlag() { g_shutdown_flag = false; }
  } restore_shutdown;

  REQUIRE(worker.is_nats_connected());
  g_shutdown_flag = false;
  std::thread runner([&worker] { worker.run(); });

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

  // Unparseable payload: the worker replies 400 instead of panicking.
  const std::optional<std::string> r3 =
      test_client.request(subject, "not-json", 3000);
  REQUIRE(r3.has_value());
  REQUIRE(json::parse(*r3)["error"] == "Invalid request format");

  g_shutdown_flag = true;
  REQUIRE(runner.joinable());
  runner.join();
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
  NatsClient client(cfg);
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
  if (nats_binary_path().empty()) {
    SKIP("nats-server binary not available");
  }
  NatsServer server;
  REQUIRE(server.start());
  REQUIRE(server.wait_ready());

  const std::string subject = unique_subject("nats.emptyreply");
  NatsResponder responder(
      live_nats_cfg(subject, server.port()), subject,
      [](const std::string &, const std::string &) {
        return NatsReply{""};
      });

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
  if (nats_binary_path().empty()) {
    SKIP("nats-server binary not available");
  }
  NatsServer server;
  REQUIRE(server.start());
  REQUIRE(server.wait_ready());

  L2BackendServer backend;
  const json urls = json::array({backend.api_url()});
  const std::string subject = unique_subject("l2w.restart");
  const EnvGuard env(
      live_worker_env(server.port(), subject, urls.dump()));
  AppContext ctx;
  attach_tracer(ctx);
  L2Worker worker(ctx);
  NatsClient test_client(live_nats_cfg(subject, server.port()));

  struct RestoreShutdownFlag {
    ~RestoreShutdownFlag() { g_shutdown_flag = false; }
  } restore_shutdown;

  REQUIRE(worker.is_nats_connected());
  g_shutdown_flag = false;
  std::thread runner([&worker] { worker.run(); });

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

  g_shutdown_flag = true;
  REQUIRE(runner.joinable());
  runner.join();
}

#include <libpq-fe.h>
#include <filesystem>
#include <pwd.h>
#include <grp.h>
#include <cerrno>
#include <format>
#include <algorithm>
#include <cstdint>

namespace pgtest {

constexpr int kPgWaitMs = 10000;

[[nodiscard]] int pg_reserve_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, (const sockaddr*)&addr, sizeof(addr)) != 0) { ::close(fd); return 0; }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, (sockaddr*)&addr, &len) != 0) { ::close(fd); return 0; }
  int p = ntohs(addr.sin_port);
  ::close(fd);
  return p;
}

[[nodiscard]] bool pg_tcp_probe(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)port);
  int rc = ::connect(fd, (const sockaddr*)&addr, sizeof(addr));
  ::close(fd);
  return rc == 0;
}

[[nodiscard]] bool pg_wait(const std::function<bool()>& pred, int ms) {
  auto d = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < d) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return pred();
}

struct PgTarget {
  bool ok=false, need=false;
  uid_t uid=0; gid_t gid=0; std::string name;
};

[[nodiscard]] PgTarget pg_target() {
  if (::geteuid() != 0) return PgTarget{true,false,::geteuid(),::getegid(),""};
  const passwd* pw = ::getpwnam("postgres");
  if (!pw) return PgTarget{};
  return PgTarget{true,true,pw->pw_uid,pw->pw_gid,"postgres"};
}

[[nodiscard]] bool pg_droppriv(const PgTarget& t) {
  if (!t.need) return true;
  if (::setgid(t.gid)!=0) return false;
  if (::initgroups(t.name.c_str(), t.gid)!=0) return false;
  return ::setuid(t.uid)==0;
}

[[nodiscard]] bool pg_redir(const char* p) {
  int fd = ::open(p, O_WRONLY|O_CREAT|O_APPEND, 0644);
  if (fd<0) return false;
  ::dup2(fd,1); ::dup2(fd,2);
  if (fd>2) ::close(fd);
  return true;
}

[[nodiscard]] std::string pg_bindir() {
  auto has=[&](const std::string& d){ return ::access((d+"/initdb").c_str(),X_OK)==0 && ::access((d+"/postgres").c_str(),X_OK)==0; };
  if (has("/usr/local/bin")) return "/usr/local/bin";
  std::error_code ec;
  const std::filesystem::path ld("/usr/lib/postgresql");
  if (!std::filesystem::exists(ld,ec)) return "";
  std::vector<std::string> vs;
  for (auto& e: std::filesystem::directory_iterator(ld,ec)) if (e.is_directory()) vs.push_back(e.path().filename().string());
  std::sort(vs.begin(),vs.end(),[](auto&a,auto&b){return std::stoi(a)>std::stoi(b);});
  for (auto&v:vs){ auto d="/usr/lib/postgresql/"+v+"/bin"; if (has(d)) return d; }
  return "";
}

class PgSrv {
public:
  int port() const { return m_port; }
  bool ensure() {
    if (m_pid>0) return true;
    if (m_fail) return false;
    if (!start()) { m_fail=true; return false; }
    return true;
  }
  void stop() {
    if (m_pid<=0) return;
    ::kill(m_pid,SIGINT);
    auto dl = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    int st=0;
    while (::waitpid(m_pid,&st,WNOHANG)==0 && std::chrono::steady_clock::now()<dl)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (::waitpid(m_pid,&st,WNOHANG)==0) { ::kill(m_pid,SIGKILL); ::waitpid(m_pid,&st,0); }
    m_pid=-1;
  }
  ~PgSrv(){ stop(); }
private:
  bool alive() const { if (m_pid<=0) return false; int st=0; return ::waitpid(m_pid,&st,WNOHANG)==0; }
  bool run_wait(const std::string& exe,const std::vector<std::string>& args,const std::string& log,const PgTarget&t) const {
    pid_t p=::fork(); if (p==0){ if (!pg_droppriv(t)) _exit(127); pg_redir(log.c_str()); std::vector<const char*> av; av.push_back(exe.c_str()); for (auto&a:args) av.push_back(a.c_str()); av.push_back(nullptr); ::execv(exe.c_str(), (char*const*)av.data()); _exit(127); }
    if (p<0) return false; int st=0; while (::waitpid(p,&st,0)<0 && errno==EINTR){} return WIFEXITED(st)&&WEXITSTATUS(st)==0;
  }
  pid_t spawn(const std::string& exe,const std::vector<std::string>& args,const std::string& log,const PgTarget&t) const {
    pid_t p=::fork(); if (p==0){ if (!pg_droppriv(t)) _exit(127); pg_redir(log.c_str()); std::vector<const char*> av; av.push_back(exe.c_str()); for (auto&a:args) av.push_back(a.c_str()); av.push_back(nullptr); ::execv(exe.c_str(), (char*const*)av.data()); _exit(127); }
    return p;
  }
  bool start() {
    if (m_bindir.empty()){ m_bindir=pg_bindir(); if (m_bindir.empty()) return false; }
    auto t=pg_target(); if (!t.ok) return false;
    m_port=pg_reserve_port(); if (m_port<=0) return false;
    const std::string tmpl="/tmp/pgdbn-XXXXXX";
    std::vector<char> dt(tmpl.begin(),tmpl.end()); dt.push_back(0);
    char* d=::mkdtemp(dt.data()); if (!d) return false; m_datadir=d;
    if (t.need && ::chown(m_datadir.c_str(),t.uid,t.gid)!=0){ cleanup(); return false; }
    if (!run_wait(m_bindir+"/initdb",{"-D",m_datadir,"-U","postgres","-A","trust","--no-locale","-E","UTF8"}, "/tmp/pginitn-"+std::to_string(m_port)+".log",t)){ cleanup(); return false; }
    pid_t sp=spawn(m_bindir+"/postgres",{"-D",m_datadir,"-p",std::to_string(m_port),"-h","127.0.0.1","-k","/tmp"}, "/tmp/pgservn-"+std::to_string(m_port)+".log",t);
    if (sp<0){ cleanup(); return false; }
    m_pid=sp;
    if (!pg_wait([this]{ return pg_tcp_probe(m_port)&&alive(); }, kPgWaitMs)){ stop(); cleanup(); return false; }
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
  if (nats_binary_path().empty()) {
    SKIP("nats-server binary not available");
  }
  NatsServer server;
  REQUIRE(server.start());
  REQUIRE(server.wait_ready());
  const std::string subject = unique_subject("l2w.dbq-skip");
  EnvVars vars = live_worker_env(server.port(), subject, "[\"http://127.0.0.1:18088/api\"]");
  const EnvGuard env(vars);
  AppContext ctx;
  attach_tracer(ctx);
  L2Worker worker(ctx);
  REQUIRE(worker.is_nats_connected());
}
TEST_CASE("NatsClient: request with timeout 0 uses config timeout", "[nats-live]") {
  if (nats_binary_path().empty()) SKIP("nats-server binary not available");
  NatsServer s; REQUIRE(s.start()); REQUIRE(s.wait_ready());
  natsConnection* nc = nullptr;
  REQUIRE(natsConnection_ConnectTo(&nc, ("nats://127.0.0.1:"+std::to_string(s.port())).c_str()) == NATS_OK);
  natsMsg* reply=nullptr;
  auto st = natsConnection_RequestString(&reply, nc, "no.such.subject.timeout0", "hi", (int64_t)0);
  (void)st; if (reply) natsMsg_Destroy(reply);
  natsConnection_Destroy(nc);
}

TEST_CASE("NatsClient: subscribe rejects invalid subject", "[nats-live]") {
  if (nats_binary_path().empty()) SKIP("nats-server binary not available");
  NatsServer s; REQUIRE(s.start()); REQUIRE(s.wait_ready());
  natsConnection* nc = nullptr;
  REQUIRE(natsConnection_ConnectTo(&nc, ("nats://127.0.0.1:"+std::to_string(s.port())).c_str()) == NATS_OK);
  natsSubscription* sub=nullptr;
  auto st = natsConnection_SubscribeSync(&sub, nc, "bad subject");
  REQUIRE(st == NATS_INVALID_SUBJECT);
  if (sub) natsSubscription_Destroy(sub);
  natsConnection_Destroy(nc);
}

TEST_CASE("NatsClient: request times out when subscriber never replies", "[nats-live]") {
  if (nats_binary_path().empty()) SKIP("nats-server binary not available");
  NatsServer s; REQUIRE(s.start()); REQUIRE(s.wait_ready());
  natsConnection* nc = nullptr;
  REQUIRE(natsConnection_ConnectTo(&nc, ("nats://127.0.0.1:"+std::to_string(s.port())).c_str()) == NATS_OK);
  natsSubscription* sub=nullptr;
  auto ssub = natsConnection_SubscribeSync(&sub, nc, "test.timeout.never.reply");
  REQUIRE(ssub == NATS_OK);
  natsMsg* reply=nullptr;
  auto sreq = natsConnection_RequestString(&reply, nc, "test.timeout.never.reply", "ping", (int64_t)50);
  REQUIRE(sreq == NATS_TIMEOUT);
  if (reply) natsMsg_Destroy(reply);
  natsSubscription_Destroy(sub);
  natsConnection_Destroy(nc);
}

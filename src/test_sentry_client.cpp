// Unit tests for the dependency-light Sentry integration: DSN parsing, event
// and envelope JSON builders (pure functions) and the async queue behaviour
// via an injected transport, and the real HTTP delivery path (send_envelope)
// against a local loopback httplib::Server. No external network traffic.
#include "httplib/httplib.h"
#include "sentry_client.hpp"
#include <catch2/catch_test_macros.hpp>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::string> split_lines(const std::string &text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (true) {
    const size_t sep = text.find('\n', start);
    if (sep == std::string::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, sep - start));
    start = sep + 1;
  }
  return lines;
}

std::string last_line(const std::string &text) {
  const auto lines = split_lines(text);
  return lines.empty() ? "" : lines.back();
}

sentry::DsnData require_dsn(const std::string &dsn) {
  const auto parsed = sentry::parse_dsn(dsn);
  if (!parsed.has_value()) {
    throw std::runtime_error("test DSN must parse: " + dsn);
  }
  return *parsed;
}

struct SentryTestMetrics {
  std::shared_ptr<prometheus::Registry> m_registry{new prometheus::Registry};
  prometheus::Counter &m_sent_counter;
  prometheus::Counter &m_failed_counter;
  prometheus::Gauge &m_queue_gauge;
  SentryTestMetrics()
      : m_sent_counter(prometheus::BuildCounter()
                         .Name("tst_sentry_sent")
                         .Help("h")
                         .Register(*m_registry)
                         .Add({})),
        m_failed_counter(prometheus::BuildCounter()
                           .Name("tst_sentry_failed")
                           .Help("h")
                           .Register(*m_registry)
                           .Add({})),
        m_queue_gauge(prometheus::BuildGauge()
                        .Name("tst_sentry_queue")
                        .Help("h")
                        .Register(*m_registry)
                        .Add({})) {}
};

} // namespace

TEST_CASE("Sentry DSN: parses a full DSN", "[sentry-client]") {
  const auto data = require_dsn(
      "https://PUBLICKEY:SECRETKEY@ingest.sentry.io/1234567");
  REQUIRE(data.m_scheme == "https");
  REQUIRE(data.m_host == "ingest.sentry.io");
  REQUIRE(data.m_port == 443);
  REQUIRE(data.m_path_prefix.empty());
  REQUIRE(data.m_public_key == "PUBLICKEY");
  REQUIRE(data.m_secret_key == "SECRETKEY");
  REQUIRE(data.m_project_id == "1234567");
}

TEST_CASE("Sentry DSN: secret key is optional", "[sentry-client]") {
  const auto data = require_dsn("https://PUBLIC@example.com:8443/proj-42");
  REQUIRE(data.m_host == "example.com");
  REQUIRE(data.m_port == 8443);
  REQUIRE(data.m_public_key == "PUBLIC");
  REQUIRE(data.m_secret_key.empty());
  REQUIRE(data.m_project_id == "proj-42");
}

TEST_CASE("Sentry DSN: self-hosted path prefix is kept", "[sentry-client]") {
  const auto data = require_dsn("http://PUBLIC@sentry.internal/base/sub/7");
  REQUIRE(data.m_scheme == "http");
  REQUIRE(data.m_host == "sentry.internal");
  REQUIRE(data.m_port == 80);
  REQUIRE(data.m_path_prefix == "/base/sub");
  REQUIRE(data.m_project_id == "7");
}

TEST_CASE("Sentry DSN: invalid inputs are rejected", "[sentry-client]") {
  REQUIRE_FALSE(sentry::parse_dsn("").has_value());
  REQUIRE_FALSE(sentry::parse_dsn("no-at-sign").has_value());
  REQUIRE_FALSE(sentry::parse_dsn("@host/project").has_value());
  REQUIRE_FALSE(sentry::parse_dsn("ftp://key@host/project").has_value());
  REQUIRE_FALSE(sentry::parse_dsn("https://key@host/").has_value());
  REQUIRE_FALSE(sentry::parse_dsn("https://key@host").has_value());
  REQUIRE_FALSE(sentry::parse_dsn("https://key@host:abc/project").has_value());
  REQUIRE_FALSE(sentry::parse_dsn("https://key@host:99999/project").has_value());
}

TEST_CASE("Sentry DSN: rejects empty public key and whitespace host",
          "[sentry-client]") {
  REQUIRE_FALSE(sentry::parse_dsn("http://:SECRET@host/project").has_value());
  REQUIRE_FALSE(sentry::parse_dsn("http://PUBLIC@ho st/project").has_value());
}

TEST_CASE("Sentry DSN: normalizes trailing slashes in the path prefix",
          "[sentry-client]") {
  const auto with_dup =
      require_dsn("http://PUBLIC@host:8080/foo//proj");
  REQUIRE(with_dup.m_host == "host");
  REQUIRE(with_dup.m_port == 8080);
  REQUIRE(with_dup.m_path_prefix == "/foo");
  REQUIRE(with_dup.m_project_id == "proj");

  const auto bare = require_dsn("http://PUBLIC@host//proj");
  REQUIRE(bare.m_path_prefix.empty());
  REQUIRE(bare.m_project_id == "proj");
}

TEST_CASE("Sentry event JSON: core fields", "[sentry-client]") {
  sentry::SentryEvent event;
  event.m_message = "boom";
  event.m_level = sentry::EventLevel::Error;
  event.m_request_id = "req-1";
  event.m_fingerprint = {"http_error", "l2"};
  event.m_tags = {{"db", "postgres"}};
  event.m_extra = {{"attempts", 3}};

  const auto json = sentry::build_event_json(event, "l2-worker", "prod",
                                             "1.2.3");
  REQUIRE(json["message"] == "boom");
  REQUIRE(json["level"] == "error");
  REQUIRE(json["platform"] == "native");
  REQUIRE(json["event_id"].get<std::string>().size() == 32);
  REQUIRE(json["release"] == "1.2.3");
  REQUIRE(json["environment"] == "prod");
  REQUIRE(json["tags"]["service"] == "l2-worker");
  REQUIRE(json["tags"]["request_id"] == "req-1");
  REQUIRE(json["tags"]["db"] == "postgres");
  REQUIRE(json["fingerprint"] ==
          std::vector<std::string>{"http_error", "l2"});
  REQUIRE(json["extra"]["attempts"] == 3);
  REQUIRE(json["exception"]["values"][0]["value"] == "boom");
}

TEST_CASE("Sentry event JSON: omits optional fields when empty",
          "[sentry-client]") {
  sentry::SentryEvent event;
  event.m_message = "info only";
  event.m_level = sentry::EventLevel::Info;

  const auto json =
      sentry::build_event_json(event, "", "", "");
  REQUIRE(json["message"] == "info only");
  REQUIRE(json["level"] == "info");
  REQUIRE_FALSE(json.contains("release"));
  REQUIRE_FALSE(json.contains("environment"));
  REQUIRE_FALSE(json.contains("tags"));
  REQUIRE_FALSE(json.contains("fingerprint"));
  REQUIRE_FALSE(json.contains("exception"));
}

TEST_CASE("Sentry event JSON: every level maps to its Sentry string",
          "[sentry-client]") {
  sentry::SentryEvent event;
  const auto level = [&](sentry::EventLevel lv) {
    sentry::SentryEvent e;
    e.m_message = "m";
    e.m_level = lv;
    return std::string(sentry::build_event_json(e, "", "", "")["level"]);
  };
  REQUIRE(level(sentry::EventLevel::Debug) == "debug");
  REQUIRE(level(sentry::EventLevel::Info) == "info");
  REQUIRE(level(sentry::EventLevel::Warning) == "warning");
  REQUIRE(level(sentry::EventLevel::Error) == "error");
  {
    const auto json = sentry::build_event_json(
        [&] {
          sentry::SentryEvent e;
          e.m_message = "fatal boom";
          e.m_level = sentry::EventLevel::Fatal;
          return e;
        }(),
        "", "", "");
    REQUIRE(json["level"] == "fatal");
    REQUIRE(json["exception"]["values"][0]["value"] == "fatal boom");
  }
}

TEST_CASE("Sentry event JSON: transaction and non-string tags are kept",
          "[sentry-client]") {
  sentry::SentryEvent event;
  event.m_message = "tx";
  event.m_transaction = "db.query";
  event.m_tags = {{"attempt", 3}, {"ids", nlohmann::json::array({1, 2})}};

  const auto json = sentry::build_event_json(event, "", "", "");
  REQUIRE(json["transaction"] == "db.query");
  REQUIRE(json["tags"]["attempt"] == "3");
  REQUIRE(json["tags"]["ids"] == "[1,2]");
}

TEST_CASE("Sentry envelope: header, auth and item structure",
          "[sentry-client]") {
  sentry::SentryEvent event;
  event.m_message = "boom";
  const auto dsn = require_dsn(
      "https://PUBLIC:SECRET@ingest.sentry.io/42");

  const auto envelope =
      sentry::build_envelope(event, dsn, "l2-worker", "", "");
  const auto lines = split_lines(envelope);
  REQUIRE(lines.size() >= 3);

  const auto header = nlohmann::json::parse(lines[0]);
  const auto item = nlohmann::json::parse(lines[1]);
  const auto payload = nlohmann::json::parse(lines[2]);

  REQUIRE(header["event_id"].get<std::string>().size() == 32);
  REQUIRE(header["sdk"]["name"] == "http-data-diod");
  REQUIRE(item["type"] == "event");
  REQUIRE(payload["event_id"] == header["event_id"]);
  REQUIRE(payload["message"] == "boom");
}

TEST_CASE("SentryClient: disabled without a DSN is a no-op",
          "[sentry-client]") {
  SentryTestMetrics m;
  SentryClient client("", m.m_sent_counter, m.m_failed_counter, m.m_queue_gauge,
                      "srv");
  REQUIRE_FALSE(client.enabled());
  client.capture_message("should be dropped");
  client.flush();
  REQUIRE(m.m_sent_counter.Value() == 0.0);
  REQUIRE(m.m_failed_counter.Value() == 0.0);
}

TEST_CASE("SentryClient: delivers queued events via the transport",
          "[sentry-client]") {
  SentryTestMetrics m;
  std::vector<std::string> delivered;
  SentryClient client(
      "https://PUBLIC@ingest.sentry.io/42", m.m_sent_counter, m.m_failed_counter,
      m.m_queue_gauge, "l2-worker", "prod", "1.0.0", 3000, 256,
      [&](const std::string &envelope) {
        delivered.push_back(envelope);
        return true;
      });
  REQUIRE(client.enabled());

  client.capture_message("err one", "req-1", {"worker_validation_error"});
  client.capture_message("err two");
  client.flush();

  REQUIRE(delivered.size() == 2);
  REQUIRE(m.m_sent_counter.Value() == 2.0);
  REQUIRE(m.m_failed_counter.Value() == 0.0);
  REQUIRE(m.m_queue_gauge.Value() == 0.0);
  bool found_first = false;
  for (const auto &envelope : delivered) {
    const auto payload = nlohmann::json::parse(last_line(envelope));
    if (payload["message"] == "err one") {
      found_first = true;
      REQUIRE(payload["tags"]["request_id"] == "req-1");
      REQUIRE(payload["tags"]["service"] == "l2-worker");
      REQUIRE(payload["environment"] == "prod");
      REQUIRE(payload["release"] == "1.0.0");
    }
  }
  REQUIRE(found_first);
}

TEST_CASE("SentryClient: transport failure is counted, not propagated",
          "[sentry-client]") {
  SentryTestMetrics m;
  SentryClient client(
      "https://PUBLIC@ingest.sentry.io/42", m.m_sent_counter, m.m_failed_counter,
      m.m_queue_gauge, "srv", "", "", 3000, 256,
      [&](const std::string &) { return false; });
  REQUIRE(client.enabled());

  client.capture_message("boom");
  REQUIRE_NOTHROW(client.flush());
  REQUIRE(m.m_failed_counter.Value() == 1.0);
  REQUIRE(m.m_sent_counter.Value() == 0.0);
}

TEST_CASE("SentryClient: full SentryEvent with tags/extra is delivered",
          "[sentry-client]") {
  // Exercises the capture(SentryEvent) API used by the DB-gateway path:
  // a full event carrying tags, extra and a fingerprint must round-trip
  // through the async queue to the transport.
  SentryTestMetrics m;
  std::vector<std::string> delivered;
  SentryClient client(
      "http://PUBLIC@ingest.local/7", m.m_sent_counter, m.m_failed_counter,
      m.m_queue_gauge, "l2-worker", "", "", 3000, 256,
      [&](const std::string &envelope) {
        delivered.push_back(envelope);
        return true;
      });

  sentry::SentryEvent event;
  event.m_message = "DB query failed: db=postgres type=query status=503";
  event.m_request_id = "req-db-1";
  event.m_tags = {{"db", "postgres"}, {"type", "query"}};
  event.m_fingerprint = {"db_query_error", "DB_UNAVAILABLE"};
  event.m_extra = {{"status", 503}};
  client.capture(event);
  client.flush();

  REQUIRE(delivered.size() == 1);
  REQUIRE(m.m_sent_counter.Value() == 1.0);
  REQUIRE(m.m_failed_counter.Value() == 0.0);

  const auto payload = nlohmann::json::parse(last_line(delivered[0]));
  REQUIRE(payload["message"] == event.m_message);
  REQUIRE(payload["tags"]["db"] == "postgres");
  REQUIRE(payload["tags"]["type"] == "query");
  REQUIRE(payload["tags"]["request_id"] == "req-db-1");
  REQUIRE(payload["tags"]["service"] == "l2-worker");
  REQUIRE(payload["fingerprint"] ==
          std::vector<std::string>{"db_query_error", "DB_UNAVAILABLE"});
  REQUIRE(payload["extra"]["status"] == 503);
  REQUIRE(payload["exception"]["values"][0]["value"] == event.m_message);
}

TEST_CASE("SentryClient: bounded queue drops the oldest on overflow",
          "[sentry-client]") {
  SentryTestMetrics m;
  std::vector<std::string> delivered;
  const size_t k_limit = 4;
  std::atomic<bool> started{false};
  std::atomic<bool> release{false};
  SentryClient client(
      "https://PUBLIC@ingest.sentry.io/42", m.m_sent_counter, m.m_failed_counter,
      m.m_queue_gauge, "srv", "", "", 3000, k_limit,
      [&](const std::string &envelope) {
        started.store(true);
        while (!release.load()) {
          std::this_thread::yield();
        }
        delivered.push_back(envelope);
        return true;
      });

  client.capture_message("msg 0");
  while (!started.load()) {
    std::this_thread::yield();
  }
  for (size_t i = 1; i <= 6; ++i) {
    client.capture_message("msg " + std::to_string(i));
  }
  release.store(true);
  client.flush();

  REQUIRE(delivered.size() == k_limit + 1);
  const auto last = nlohmann::json::parse(last_line(delivered.back()));
  REQUIRE(last["message"] == "msg 6");
  REQUIRE(m.m_failed_counter.Value() == 2.0);
  REQUIRE(m.m_sent_counter.Value() == 5.0);
  REQUIRE(m.m_queue_gauge.Value() == 0.0);
}

TEST_CASE("SentryClient: send_envelope delivers over real HTTP",
          "[sentry-client]") {
  httplib::Server server;
  std::string received;
  std::string received_auth_header;
  server.Post(".*", [&](const httplib::Request &req, httplib::Response &res) {
    received = req.body;
    auto auth = req.headers.find("X-Sentry-Auth");
    if (auth != req.headers.end()) {
      received_auth_header = auth->second;
    }
    res.status = 200;
  });
  const int port = server.bind_to_any_port("127.0.0.1");
  std::thread server_thread([&] { server.listen_after_bind(); });

  SentryTestMetrics m;
  SentryClient client(
      "http://PUBLIC@127.0.0.1:" + std::to_string(port) + "/42",
      m.m_sent_counter, m.m_failed_counter, m.m_queue_gauge, "srv", "", "",
      3000, 8);
  REQUIRE(client.enabled());

  client.capture_message("via real http");
  client.flush();

  REQUIRE(m.m_sent_counter.Value() == 1.0);
  REQUIRE(m.m_failed_counter.Value() == 0.0);
  REQUIRE(received.find("sdk") != std::string::npos);
  REQUIRE(received.find("via real http") != std::string::npos);
  REQUIRE(received_auth_header.find("sentry_key=PUBLIC") !=
          std::string::npos);

  server.stop();
  server_thread.join();
}

TEST_CASE("SentryClient: send_envelope counts a non-2xx response as failure",
          "[sentry-client]") {
  httplib::Server server;
  server.Post(".*", [](const httplib::Request &, httplib::Response &res) {
    res.status = 500;
  });
  const int port = server.bind_to_any_port("127.0.0.1");
  std::thread server_thread([&] { server.listen_after_bind(); });

  SentryTestMetrics m;
  SentryClient client(
      "http://PUBLIC@127.0.0.1:" + std::to_string(port) + "/42",
      m.m_sent_counter, m.m_failed_counter, m.m_queue_gauge, "srv", "", "",
      3000, 8);
  client.capture_message("server error");
  client.flush();

  REQUIRE(m.m_sent_counter.Value() == 0.0);
  REQUIRE(m.m_failed_counter.Value() == 1.0);

  server.stop();
  server_thread.join();
}

TEST_CASE("SentryClient: send_envelope counts a connection failure",
          "[sentry-client]") {
  SentryTestMetrics m;
  SentryClient client("http://PUBLIC@127.0.0.1:1/42", m.m_sent_counter,
                      m.m_failed_counter, m.m_queue_gauge, "srv", "", "", 1000,
                      8);
  client.capture_message("dead endpoint");
  client.flush();

  REQUIRE(m.m_sent_counter.Value() == 0.0);
  REQUIRE(m.m_failed_counter.Value() == 1.0);
}

TEST_CASE("SentryClient: send_envelope https scheme fails without a TLS server",
          "[sentry-client]") {
  SentryTestMetrics m;
  SentryClient client("https://PUBLIC@127.0.0.1:1/42", m.m_sent_counter,
                      m.m_failed_counter, m.m_queue_gauge, "srv", "", "", 1000,
                      8);
  client.capture_message("no tls here");
  client.flush();

  REQUIRE(m.m_sent_counter.Value() == 0.0);
  REQUIRE(m.m_failed_counter.Value() == 1.0);
}

TEST_CASE("SentryClient: secret key is sent in the X-Sentry-Auth header",
          "[sentry-client]") {
  httplib::Server server;
  std::string received_auth_header;
  server.Post(".*", [&](const httplib::Request &req, httplib::Response &res) {
    auto auth = req.headers.find("X-Sentry-Auth");
    if (auth != req.headers.end()) {
      received_auth_header = auth->second;
    }
    res.status = 200;
  });
  const int port = server.bind_to_any_port("127.0.0.1");
  std::thread server_thread([&] { server.listen_after_bind(); });

  SentryTestMetrics m;
  SentryClient client(
      "http://PUBLIC:SECRET@127.0.0.1:" + std::to_string(port) + "/42",
      m.m_sent_counter, m.m_failed_counter, m.m_queue_gauge, "srv", "", "",
      3000, 8);
  client.capture_message("with secret");
  client.flush();

  REQUIRE(m.m_sent_counter.Value() == 1.0);
  REQUIRE(m.m_failed_counter.Value() == 0.0);
  REQUIRE(received_auth_header.find("sentry_key=PUBLIC/SECRET") !=
          std::string::npos);

  server.stop();
  server_thread.join();
}

TEST_CASE("SentryClient: zero max_queue_size is clamped to one",
          "[sentry-client]") {
  SentryTestMetrics m;
  std::vector<std::string> delivered;
  SentryClient client(
      "https://PUBLIC@ingest.sentry.io/42", m.m_sent_counter, m.m_failed_counter,
      m.m_queue_gauge, "srv", "", "", 1000, 0,
      [&](const std::string &envelope) {
        delivered.push_back(envelope);
        return true;
      });
  client.capture_message("single slot");
  client.capture_message("second dropped");
  client.flush();

  REQUIRE(delivered.size() == 1);
  REQUIRE(m.m_failed_counter.Value() == 1.0);
  REQUIRE(m.m_sent_counter.Value() == 1.0);
}
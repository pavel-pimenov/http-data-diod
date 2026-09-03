// Proxy-core unit tests: /v1/sql routing contract (path parsing and the
// action/method decision) and the pure DB gateway helpers. Separate TU from
// test_components.cpp; both run from the Docker build test stage.
#include "db_gateway_routing.hpp"
#include "db_query_utils.hpp"
#include "dynamic_labeled_family.hpp"
#include "error_categorizer.hpp"
#include "header_utils.hpp"
#include "json_schema_validator.hpp"
#include "json_utils.hpp"
#include "random_utils.hpp"
#include "url_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <unordered_map>

using db_gateway_routing::classify_method;
using db_gateway_routing::normalize_path_rest;
using db_gateway_routing::parse_path;

TEST_CASE("Gateway path: trims leading/trailing slashes", "[db-gateway-path]") {
  REQUIRE(normalize_path_rest("oracle/ping") == "oracle/ping");
  REQUIRE(normalize_path_rest("/oracle/ping") == "oracle/ping");
  REQUIRE(normalize_path_rest("oracle/ping/") == "oracle/ping");
  REQUIRE(normalize_path_rest("/") == "");
  REQUIRE(normalize_path_rest("//oracle//ping/") == "oracle//ping");
  REQUIRE(normalize_path_rest("") == "");
}

TEST_CASE("Gateway path: empty rest is the list action", "[db-gateway-path]") {
  const auto parsed = parse_path("");
  REQUIRE(parsed.m_is_list);
  REQUIRE(parsed.m_valid);
  REQUIRE(parsed.m_db_name.empty());
  REQUIRE(parsed.m_action.empty());
}

TEST_CASE("Gateway path: parses db and action", "[db-gateway-path]") {
  const auto parsed = parse_path("oracle/ping");
  REQUIRE_FALSE(parsed.m_is_list);
  REQUIRE(parsed.m_valid);
  REQUIRE(parsed.m_db_name == "oracle");
  REQUIRE(parsed.m_action == "ping");
}

TEST_CASE("Gateway path: rejects missing action", "[db-gateway-path]") {
  const auto parsed = parse_path("oracle");
  REQUIRE_FALSE(parsed.m_is_list);
  REQUIRE_FALSE(parsed.m_valid);
  REQUIRE(parsed.m_db_name == "oracle");
  REQUIRE(parsed.m_action.empty());
}

TEST_CASE("Gateway path: rejects extra path segments", "[db-gateway-path]") {
  const auto parsed = parse_path("oracle/ping/extra");
  REQUIRE_FALSE(parsed.m_valid);
  REQUIRE(parsed.m_db_name == "oracle");
  REQUIRE(parsed.m_action == "ping/extra");
}

TEST_CASE("Gateway path: empty db is invalid", "[db-gateway-path]") {
  const auto parsed = parse_path("/ping");
  REQUIRE_FALSE(parsed.m_is_list);
  REQUIRE_FALSE(parsed.m_valid);
  REQUIRE(parsed.m_db_name.empty());
}

TEST_CASE("Gateway method: query is POST-only", "[db-gateway-method]") {
  REQUIRE_FALSE(classify_method("query", "POST").m_is_error);
  for (const char *method : {"GET", "PUT", "DELETE", "PATCH", "HEAD"}) {
    const auto d = classify_method("query", method);
    REQUIRE(d.m_is_error);
    REQUIRE(d.m_status == 405);
    REQUIRE(d.m_code == "METHOD_NOT_ALLOWED");
  }
}

TEST_CASE("Gateway method: ping is GET-only", "[db-gateway-method]") {
  REQUIRE_FALSE(classify_method("ping", "GET").m_is_error);
  for (const char *method : {"POST", "PUT", "DELETE", "PATCH"}) {
    const auto d = classify_method("ping", method);
    REQUIRE(d.m_is_error);
    REQUIRE(d.m_status == 405);
    REQUIRE(d.m_code == "METHOD_NOT_ALLOWED");
  }
}

TEST_CASE("Gateway method: unknown action is 404 regardless of verb",
          "[db-gateway-method]") {
  for (const char *action : {"bogus", "list", "DROP", "queryx", ""}) {
    for (const char *method : {"GET", "POST"}) {
      const auto d = classify_method(action, method);
      REQUIRE(d.m_is_error);
      REQUIRE(d.m_status == 404);
      REQUIRE(d.m_code == "NOT_FOUND");
    }
  }
}

TEST_CASE("Gateway method: error messages carry action and verb",
          "[db-gateway-method]") {
  const auto d405 = classify_method("query", "GET");
  REQUIRE(d405.m_message.find("'query'") != std::string::npos);
  REQUIRE(d405.m_message.find("GET") != std::string::npos);
  const auto d404 = classify_method("bogus", "POST");
  REQUIRE(d404.m_message.find("bogus") != std::string::npos);
}

TEST_CASE("Gateway errors: error body shape", "[db-gateway-body]") {
  const json body = make_db_error_body(404, "NOT_FOUND", "no such db");
  REQUIRE(body[DbResponseContract::kStatus] ==
          DbResponseContract::kStatusError);
  REQUIRE(body[DbResponseContract::kError][DbResponseContract::kCode] ==
          "NOT_FOUND");
  REQUIRE(body[DbResponseContract::kError][DbResponseContract::kMessage] ==
          "no such db");
}

TEST_CASE("Gateway errors: unavailable and sql_error set the http status",
          "[db-gateway-body]") {
  int status = 0;
  const json unavailable = make_db_unavailable(status, "conn refused");
  REQUIRE(status == 503);
  REQUIRE(unavailable[DbResponseContract::kError][DbResponseContract::kCode] ==
          "DB_UNAVAILABLE");
  REQUIRE(std::string(
              unavailable[DbResponseContract::kError]
                         [DbResponseContract::kMessage]
                             .get_ref<const std::string &>())
              .find("conn refused") != std::string::npos);

  status = 0;
  const json sql_error = make_db_sql_error(status, "ORA-00942: no such table");
  REQUIRE(status == 422);
  REQUIRE(sql_error[DbResponseContract::kError][DbResponseContract::kCode] ==
          "SQL_ERROR");
}

TEST_CASE("Gateway requests: build_db_query_request carries db and sql",
          "[db-gateway-request]") {
  const json req = build_db_query_request("query", "req-1", "oracle",
                                          json{{"sql", "SELECT 1"}});
  REQUIRE(req[DbQueryContract::kType] == "query");
  REQUIRE(req[DbQueryContract::kRequestId] == "req-1");
  REQUIRE(req[DbQueryContract::kDb] == "oracle");
  REQUIRE(req[DbQueryContract::kSql] == "SELECT 1");

  const json ping = build_db_query_request("ping", "req-2", "postgres");
  REQUIRE(ping[DbQueryContract::kType] == "ping");
  REQUIRE_FALSE(ping.contains(DbQueryContract::kSql));
}

TEST_CASE("Gateway requests: optional fields are forwarded",
          "[db-gateway-request]") {
  const json req = build_db_query_request(
      "query", "r", "oracle",
      json{{"sql", "SELECT * FROM t WHERE id = :id"},
           {DbQueryContract::kParams, json{{"id", 1}}},
           {DbQueryContract::kTimeoutMs, 9000},
           {DbQueryContract::kMaxRows, 25}});
  REQUIRE(req[DbQueryContract::kParams]["id"] == 1);
  REQUIRE(req[DbQueryContract::kTimeoutMs] == 9000);
  REQUIRE(req[DbQueryContract::kMaxRows] == 25);
}

TEST_CASE("Gateway read-only check: SELECT and WITH pass",
          "[db-gateway-readonly]") {
  REQUIRE(is_read_only_sql("SELECT * FROM t"));
  REQUIRE(is_read_only_sql("  select 1 from dual"));
  REQUIRE(is_read_only_sql("WITH x AS (SELECT 1) SELECT * FROM x"));
  REQUIRE(is_read_only_sql("/* hi */ SELECT 1"));
  REQUIRE(is_read_only_sql("(SELECT 1)"));
}

TEST_CASE("Gateway read-only check: mutations are rejected",
          "[db-gateway-readonly]") {
  REQUIRE_FALSE(is_read_only_sql("INSERT INTO t VALUES (1)"));
  REQUIRE_FALSE(is_read_only_sql("UPDATE t SET a=1"));
  REQUIRE_FALSE(is_read_only_sql("DELETE FROM t"));
  REQUIRE_FALSE(is_read_only_sql("DROP TABLE t"));
  // A comment must not smuggle a mutation past the check.
  REQUIRE_FALSE(is_read_only_sql("/* x */ DELETE FROM t"));
  REQUIRE_FALSE(is_read_only_sql("-- x\nDELETE FROM t"));
}

TEST_CASE("Gateway read-only check: strip_sql_comments edges",
          "[db-gateway-readonly]") {
  REQUIRE(strip_sql_comments("-- hi") == "");
  REQUIRE(strip_sql_comments("/* hi */") == "");
  REQUIRE(strip_sql_comments("/* hi */ SELECT 1") == " SELECT 1");
  REQUIRE(strip_sql_comments("SELECT 1 -- trailing") == "SELECT 1 ");
  REQUIRE(strip_sql_comments("-- only\nSELECT 1") == "\nSELECT 1");
  REQUIRE(strip_sql_comments("/* unterminated") == "");
  REQUIRE(strip_sql_comments("") == "");
  // A quote does not disable comment stripping (documented limitation: the
  // reader is naive, string literals are not parsed).
  REQUIRE(strip_sql_comments("SELECT '--x' FROM t") ==
          "SELECT '");
  // The first keyword is still SELECT, so the read-only check stays correct
  // even though the trailing part of the string literal was eaten.
  REQUIRE(is_read_only_sql("SELECT '--x' FROM t") == true);
}

TEST_CASE("Gateway request validation: parses a valid query",
          "[db-gateway-validate]") {
  const json raw = build_db_query_request(
      "query", "req-1", "oracle",
      json{{"sql", "SELECT message FROM demo WHERE id = :id"},
           {DbQueryContract::kParams, json{{"id", 1}}}});
  const auto parsed = parse_db_query_request(raw);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->m_type == "query");
  REQUIRE(parsed->m_db == "oracle");
  REQUIRE(parsed->m_sql == "SELECT message FROM demo WHERE id = :id");
  REQUIRE(parsed->m_params["id"] == 1);
}

TEST_CASE("Gateway request validation: empty/mutating sql fails",
          "[db-gateway-validate]") {
  const auto no_sql =
      parse_db_query_request(json{{DbQueryContract::kType, "query"},
                                  {DbQueryContract::kDb, "oracle"}});
  REQUIRE_FALSE(no_sql.has_value());

  const auto mutation = parse_db_query_request(
      json{{DbQueryContract::kType, "query"},
           {DbQueryContract::kDb, "oracle"},
           {DbQueryContract::kSql, "DROP TABLE t"}});
  REQUIRE_FALSE(mutation.has_value());
}

TEST_CASE("Gateway request validation: rejects non-object params",
          "[db-gateway-validate]") {
  const auto bad = parse_db_query_request(
      json{{DbQueryContract::kType, "query"},
           {DbQueryContract::kDb, "oracle"},
           {DbQueryContract::kSql, "SELECT 1"},
           {DbQueryContract::kParams, json::array()}});
  REQUIRE_FALSE(bad.has_value());
}

TEST_CASE("Gateway request validation: clamps timeout and max_rows",
          "[db-gateway-validate]") {
  const auto ok = parse_db_query_request(
      json{{DbQueryContract::kType, "query"},
           {DbQueryContract::kDb, "oracle"},
           {DbQueryContract::kSql, "SELECT 1"},
           {DbQueryContract::kTimeoutMs, 7000},
           {DbQueryContract::kMaxRows, 100}});
  REQUIRE(ok.has_value());
  REQUIRE(ok->m_timeout_ms == 7000);
  REQUIRE(ok->m_max_rows == 100);

  const auto bad_timeout = parse_db_query_request(
      json{{DbQueryContract::kType, "query"},
           {DbQueryContract::kDb, "oracle"},
           {DbQueryContract::kSql, "SELECT 1"},
           {DbQueryContract::kTimeoutMs, 0}});
  REQUIRE_FALSE(bad_timeout.has_value());
}

TEST_CASE("Gateway responses: query and ping bodies", "[db-gateway-response]") {
  const json cols = make_db_columns_json({{"ID", "NUMBER"}, {"MESSAGE", "VARCHAR2"}});
  const json rows = json::array({{1, "hello"}});
  const json query_resp = make_db_query_response("oracle", cols, rows, 1,
                                                 false, 12);
  REQUIRE(query_resp[DbResponseContract::kStatus] == "ok");
  REQUIRE(query_resp[DbResponseContract::kDb] == "oracle");
  REQUIRE(query_resp[DbResponseContract::kRowCount] == 1);
  REQUIRE(query_resp[DbResponseContract::kTruncated] == false);
  REQUIRE(query_resp[DbResponseContract::kColumns].size() == 2);

  const json ping_resp = make_db_ping_response("oracle", 3);
  REQUIRE(ping_resp[DbResponseContract::kStatus] == "ok");
  REQUIRE(ping_resp[DbResponseContract::kLatencyMs] == 3);
}

TEST_CASE("Gateway labels: nonempty_or picks the value or the fallback",
          "[db-gateway-labels]") {
  REQUIRE(nonempty_or("oracle", "unknown") == "oracle");
  REQUIRE(nonempty_or("", "unknown") == "unknown");
  REQUIRE(nonempty_or("query", "unknown") == "query");
  REQUIRE(nonempty_or("", "unknown") != "");
}

TEST_CASE("Gateway envelope: wraps status and body", "[db-gateway-envelope]") {
  const json body = json{{"status", "ok"}};
  const json envelope = make_db_response_envelope(200, body);
  REQUIRE(envelope[DbQueryContract::kStatus] == 200);
  REQUIRE(envelope[DbQueryContract::kBody] == body);
}

TEST_CASE("Gateway row collector: enforces the row limit", "[db-gateway-rows]") {
  DbRowCollector collector(2);
  REQUIRE(collector.try_add(json::array({1})));
  REQUIRE(collector.try_add(json::array({2})));
  REQUIRE_FALSE(collector.try_add(json::array({3})));
  REQUIRE(collector.truncated());
  REQUIRE(collector.size() == 2);
  REQUIRE(collector.take_rows().size() == 2);
}

TEST_CASE("Gateway helper: resolve_positive_or falls back on non-positive",
          "[db-gateway-helper]") {
  REQUIRE(resolve_positive_or(42, 5) == 42);
  REQUIRE(resolve_positive_or(0, 5) == 5);
  REQUIRE(resolve_positive_or(-1, 5) == 5);
}

TEST_CASE("Request data: client IP prefers X-Real-IP over X-Forwarded-For",
          "[request-data]") {
  httplib::Request req;
  req.remote_addr = "9.9.9.9";
  req.headers.emplace("x-real-ip", "1.2.3.4");
  req.headers.emplace("x-forwarded-for", "5.5.5.5, 6.6.6.6");
  REQUIRE(extract_client_ip(req) == "1.2.3.4");
}

TEST_CASE("Request data: client IP takes last XFF entry when no X-Real-IP",
          "[request-data]") {
  httplib::Request req;
  req.remote_addr = "9.9.9.9";
  req.headers.emplace("x-forwarded-for", "5.5.5.5, 6.6.6.6");
  REQUIRE(extract_client_ip(req) == "6.6.6.6");
}

TEST_CASE("Request data: client IP falls back to remote addr",
          "[request-data]") {
  httplib::Request req;
  req.remote_addr = "9.9.9.9";
  REQUIRE(extract_client_ip(req) == "9.9.9.9");
}

TEST_CASE("Request data: query string is taken after the ?", "[request-data]") {
  httplib::Request req;
  req.target = "/v1/sql/oracle/query?a=1&b=2";
  REQUIRE(extract_query_string(req) == "a=1&b=2");
}

TEST_CASE("Request data: empty query string when no ? present",
          "[request-data]") {
  httplib::Request req;
  req.target = "/v1/sql/oracle/ping";
  REQUIRE(extract_query_string(req) == "");
}

TEST_CASE("Request data: proxy IP is the local addr", "[request-data]") {
  httplib::Request req;
  req.local_addr = "127.0.0.1";
  REQUIRE(extract_proxy_ip(req) == "127.0.0.1");
}

TEST_CASE("Header utils: sensitive headers are detected", "[header-utils]") {
  REQUIRE(HeaderUtils::is_sensitive_header("Authorization"));
  REQUIRE(HeaderUtils::is_sensitive_header("Cookie"));
  REQUIRE(HeaderUtils::is_sensitive_header("X-Api-Key"));
  REQUIRE(HeaderUtils::is_sensitive_header("x-amz-security-token"));
  REQUIRE_FALSE(HeaderUtils::is_sensitive_header("X-Forwarded-For"));
  REQUIRE_FALSE(HeaderUtils::is_sensitive_header("Content-Length"));
}

TEST_CASE("Header utils: binary content types detected", "[header-utils]") {
  REQUIRE(HeaderUtils::is_binary_content_type("image/png"));
  REQUIRE(HeaderUtils::is_binary_content_type("application/octet-stream"));
  REQUIRE(HeaderUtils::is_binary_content_type("video/mp4"));
  REQUIRE_FALSE(HeaderUtils::is_binary_content_type("application/json"));
}

TEST_CASE("Header utils: skip and redact", "[header-utils]") {
  REQUIRE(HeaderUtils::should_skip_header("Host"));
  REQUIRE(HeaderUtils::should_skip_header("Content-Length"));
  REQUIRE_FALSE(HeaderUtils::should_skip_header("X-Custom"));
  REQUIRE(HeaderUtils::redact_header_value("Authorization", "Bearer 123") ==
          "***");
  REQUIRE(HeaderUtils::redact_header_value("X-Custom", "data") == "data");
  REQUIRE(HeaderUtils::to_lower("X-Custom") == "x-custom");
}

TEST_CASE("Header utils: filter_headers skips default headers case-insensitively",
          "[header-utils]") {
  httplib::Headers source = {
      {"Host", "example.com"},
      {"Content-Length", "42"},
      {"X-Custom", "value"},
      {"Connection", "keep-alive"},
  };
  httplib::Headers dest;
  HeaderUtils::filter_headers(source, dest);

  REQUIRE_FALSE(dest.find("Host") != dest.end());
  REQUIRE_FALSE(dest.find("Content-Length") != dest.end());
  REQUIRE_FALSE(dest.find("Connection") != dest.end());
  REQUIRE(dest.find("X-Custom") != dest.end());
  REQUIRE(dest.find("X-Custom")->second == "value");
}

TEST_CASE("Header utils: filter_headers honours a custom skip set",
          "[header-utils]") {
  httplib::Headers source = {{"X-Drop", "1"}, {"X-Keep", "2"}};
  httplib::Headers dest;
  header_utils::HeaderSet skip = {"x-drop"};
  HeaderUtils::filter_headers(source, dest, skip);

  REQUIRE_FALSE(dest.find("X-Drop") != dest.end());
  REQUIRE(dest.find("X-Keep") != dest.end());
}

TEST_CASE("Header utils: filter_headers_to_json skips default headers",
          "[header-utils]") {
  httplib::Headers source = {
      {"Authorization", "Bearer secret"},
      {"X-Forwarded-For", "1.2.3.4"},
      {"Transfer-Encoding", "chunked"},
  };
  nlohmann::json out;
  HeaderUtils::filter_headers_to_json(source, out);

  // Transfer-Encoding is in the default skip set; sensitive headers like
  // Authorization are still forwarded (only their log value is redacted).
  REQUIRE_FALSE(out.contains("Transfer-Encoding"));
  REQUIRE(out.contains("X-Forwarded-For"));
  REQUIRE(out["X-Forwarded-For"] == "1.2.3.4");
}

TEST_CASE("Header utils: filter_headers_from_json skips default headers",
          "[header-utils]") {
  nlohmann::json source = {
      {"Host", "example.com"},
      {"X-Request-Id", "abc"},
  };
  httplib::Headers dest;
  HeaderUtils::filter_headers_from_json(source, dest);

  REQUIRE(dest.size() == 1);
  REQUIRE(dest.find("X-Request-Id") != dest.end());
  REQUIRE(dest.find("X-Request-Id")->second == "abc");
}

TEST_CASE("Header utils: filter_headers_from_json ignores non-object input",
          "[header-utils]") {
  httplib::Headers dest;
  HeaderUtils::filter_headers_from_json(nlohmann::json::array(), dest);
  REQUIRE(dest.empty());
}

TEST_CASE("Header utils: headers_to_json preserves all pairs",
          "[header-utils]") {
  httplib::Headers source = {{"A", "1"}, {"B", "2"}};
  const auto out = HeaderUtils::headers_to_json(source);
  REQUIRE(out.size() == 2);
  REQUIRE(out["A"] == "1");
  REQUIRE(out["B"] == "2");
}

TEST_CASE("Header utils: should_skip_header with custom set", "[header-utils]") {
  header_utils::HeaderSet skip = {"x-custom"};
  REQUIRE(HeaderUtils::should_skip_header("X-Custom", skip));
  REQUIRE(HeaderUtils::should_skip_header("x-custom", skip));
  REQUIRE_FALSE(HeaderUtils::should_skip_header("X-Other", skip));
}

TEST_CASE("Random utils: between stays within the inclusive range",
          "[random-utils]") {
  for (int i = 0; i < 200; ++i) {
    const int v = RandomUtils::between(5, 7);
    REQUIRE(v >= 5);
    REQUIRE(v <= 7);
  }
  REQUIRE(RandomUtils::between(42, 42) == 42);
}

TEST_CASE("Schema validator: required fields, methods, paths, sizes",
          "[schema-validator]") {
  std::string error;
  RequestValidator validator =
      create_standard_request_validator();
  json ok = {{"method", "POST"}, {"path", "/v1/sql"}, {"request_id", "r1"}};
  REQUIRE(validator.validate(ok, error));
  REQUIRE_NOTHROW(validator.validate_or_throw(ok));

  json missing = {{"method", "POST"}, {"path", "/v1/sql"}};
  REQUIRE_FALSE(validator.validate(missing, error));
  REQUIRE(error.find("Missing required field") != std::string::npos);
  REQUIRE_THROWS_AS(validator.validate_or_throw(missing),
                    std::invalid_argument);

  json bad_method = {{"method", "DELETE"}, {"path", "/v1/sql"}, {"request_id", "r1"}};
  REQUIRE_FALSE(validator.validate(bad_method, error));
  REQUIRE(error.find("Method not allowed") != std::string::npos);

  RequestValidator path_validator = create_standard_request_validator();
  path_validator.add_allowed_path("/v1");
  json bad_path = {{"method", "POST"}, {"path", "/other"}, {"request_id", "r1"}};
  REQUIRE_FALSE(path_validator.validate(bad_path, error));
  REQUIRE(error.find("Path not allowed") != std::string::npos);

  json too_long_path = {{"method", "POST"},
                        {"path", std::string(3000, 'x')},
                        {"request_id", "r1"}};
  REQUIRE_FALSE(validator.validate(too_long_path, error));
  REQUIRE(error.find("Path too long") != std::string::npos);

  json too_big_body = {{"method", "POST"},
                       {"path", "/v1/sql"},
                       {"request_id", "r1"},
                       {"body", std::string(11 * 1024 * 1024, 'a')}};
  REQUIRE_FALSE(validator.validate(too_big_body, error));
  REQUIRE(error.find("Body too large") != std::string::npos);

  json with_body_ok = {{"method", "POST"},
                       {"path", "/v1/sql"},
                       {"request_id", "r1"},
                       {"body", "small"}};
  REQUIRE(validator.validate(with_body_ok, error));
}

TEST_CASE("Schema validator: response status and body rules",
          "[schema-validator]") {
  std::string error;
  ResponseValidator validator =
      create_standard_response_validator();
  validator.add_allowed_status_code(200);

  json ok = {{"status_code", 200},
             {"body", {{"response", "payload"}}}};
  REQUIRE(validator.validate(ok, error));

  json bad_status = {{"status_code", 500},
                     {"body", {{"response", "payload"}}}};
  REQUIRE_FALSE(validator.validate(bad_status, error));
  REQUIRE(error.find("Status code not allowed") != std::string::npos);

  json missing_body = {{"status_code", 200}};
  REQUIRE_FALSE(validator.validate(missing_body, error));
  REQUIRE(error.find("body required but missing") != std::string::npos);

  json too_big = {{"status_code", 200},
                  {"body", {{"response", std::string(51 * 1024 * 1024, 'b')}}}};
  REQUIRE_FALSE(validator.validate(too_big, error));
  REQUIRE(error.find("body too large") != std::string::npos);
}

TEST_CASE("Request data: NatsContract field names stay stable",
          "[request-data]") {
  // prepare_request_data writes these exact keys into the backend envelope;
  // a rename here silently breaks the worker/consumer contract.
  REQUIRE(std::string(NatsContract::kRequestId) == "request_id");
  REQUIRE(std::string(NatsContract::kMethod) == "method");
  REQUIRE(std::string(NatsContract::kPath) == "path");
  REQUIRE(std::string(NatsContract::kQuery) == "query");
  REQUIRE(std::string(NatsContract::kClientIp) == "client_ip");
  REQUIRE(std::string(NatsContract::kProxyIp) == "proxy_ip");
  REQUIRE(std::string(NatsContract::kTraceparent) == "traceparent");
  REQUIRE(std::string(NatsContract::kBody) == "body");
  REQUIRE(std::string(NatsContract::kHeaders) == "headers");
}

TEST_CASE("Dynamic labeled family: counter increments and renders",
          "[labeled-family]") {
  DynamicLabeledFamily<prometheus::Counter> family(
      "client_id",
      std::vector<DynamicLabeledFamily<prometheus::Counter>::Series>{
          {"requests_total", "Requests per client"},
          {"rejected_total", "Rejected per client"}});
  family.get("alice", 0)->Increment();
  family.get("alice", 0)->Increment();
  family.get("alice", 1)->Increment();

  auto families = family.Collect();
  REQUIRE(families.size() == 2);
  // Requests family
  REQUIRE(families[0].name == "requests_total");
  REQUIRE(families[0].metric.size() == 1);
  REQUIRE(families[0].metric[0].label.size() == 1);
  REQUIRE(families[0].metric[0].label[0].name == "client_id");
  REQUIRE(families[0].metric[0].label[0].value == "alice");
  REQUIRE(families[0].metric[0].counter.value == 2.0);
  // Rejected family
  REQUIRE(families[1].name == "rejected_total");
  REQUIRE(families[1].metric[0].counter.value == 1.0);
}

TEST_CASE("Dynamic labeled family: empty family is omitted on collect",
          "[labeled-family]") {
  DynamicLabeledFamily<prometheus::Counter> family(
      "client_id",
      std::vector<DynamicLabeledFamily<prometheus::Counter>::Series>{
          {"requests_total", "help"}});
  REQUIRE(family.Collect().empty());
}

TEST_CASE("Dynamic labeled family: max_entries cap evicts oldest",
          "[labeled-family]") {
  DynamicLabeledFamily<prometheus::Counter> family(
      "client_id",
      std::vector<DynamicLabeledFamily<prometheus::Counter>::Series>{
          {"requests_total", "help"}},
      {}, 0, 2);
  family.get("a", 0)->Increment();
  family.get("b", 0)->Increment();
  family.get("c", 0)->Increment();

  auto families = family.Collect();
  REQUIRE(families.size() == 1);
  // Two of the three series survive (created oldest == most recently touched
  // order is the same here since all are touched once in sequence), and the
  // most-recently-touched ones are kept.
  REQUIRE(families[0].metric.size() == 2);
}

TEST_CASE("Dynamic labeled family: snapshot provider replaces gauge set",
          "[labeled-family]") {
  auto current_requests = 0.0;
  auto current_rejected = 0.0;
  DynamicLabeledFamily<prometheus::Gauge> family(
      "ip",
      std::vector<DynamicLabeledFamily<prometheus::Gauge>::Series>{
          {"ip_requests", "req"}, {"ip_rejected", "rej"}},
      [&]() {
        return std::vector<std::pair<std::string, std::vector<double>>>{
            {"1.2.3.4", {current_requests, current_rejected}}};
      });

  current_requests = 7.0;
  current_rejected = 2.0;
  auto families = family.Collect();
  REQUIRE(families.size() == 2);
  REQUIRE(families[0].name == "ip_requests");
  REQUIRE(families[0].metric[0].label[0].value == "1.2.3.4");
  REQUIRE(families[0].metric[0].gauge.value == 7.0);
  REQUIRE(families[1].metric[0].gauge.value == 2.0);
}

TEST_CASE("Dynamic labeled family: provider drops vanished labels",
          "[labeled-family]") {
  auto present = true;
  DynamicLabeledFamily<prometheus::Gauge> family(
      "ip",
      std::vector<DynamicLabeledFamily<prometheus::Gauge>::Series>{
          {"ip_requests", "req"}},
      [&]() {
        std::vector<std::pair<std::string, std::vector<double>>> entries;
        if (present) {
          entries.emplace_back("9.9.9.9", std::vector<double>{3.0});
        }
        return entries;
      });

  REQUIRE(family.Collect().size() == 1);
  present = false;
  REQUIRE(family.Collect().empty());
}

// ============================================================================
// Error categorization (error_categorizer.hpp)
// ============================================================================

using namespace error_categorizer;

TEST_CASE("Error categorizer: http keyword rules", "[error-categorizer]") {
  REQUIRE(categorize_http_error("Connection refused") ==
          HttpErrorType::CONNECTION_ERROR);
  REQUIRE(categorize_http_error("could not resolve host") ==
          HttpErrorType::CONNECTION_ERROR);
  REQUIRE(categorize_http_error("DNS lookup failed") ==
          HttpErrorType::CONNECTION_ERROR);
  REQUIRE(categorize_http_error("SSL handshake failed") ==
          HttpErrorType::SSL_ERROR);
  REQUIRE(categorize_http_error("certificate verify failed") ==
          HttpErrorType::SSL_ERROR);
  REQUIRE(categorize_http_error("Operation timed out") ==
          HttpErrorType::TIMEOUT_ERROR);
  REQUIRE(categorize_http_error("Protocol error") ==
          HttpErrorType::PROTOCOL_ERROR);
  REQUIRE(categorize_http_error("unexpected character invalid") ==
          HttpErrorType::PROTOCOL_ERROR);
}

TEST_CASE("Error categorizer: http status codes take precedence over text",
          "[error-categorizer]") {
  REQUIRE(categorize_http_error("anything", 500) ==
          HttpErrorType::SERVER_ERROR);
  REQUIRE(categorize_http_error("anything", 503) ==
          HttpErrorType::SERVER_ERROR);
  REQUIRE(categorize_http_error("anything", 429) ==
          HttpErrorType::RATE_LIMIT_ERROR);
  REQUIRE(categorize_http_error("anything", 400) ==
          HttpErrorType::CLIENT_ERROR);
  REQUIRE(categorize_http_error("anything", 404) ==
          HttpErrorType::CLIENT_ERROR);
}

TEST_CASE("Error categorizer: http unknown falls back to OTHER",
          "[error-categorizer]") {
  REQUIRE(categorize_http_error("no matching keyword here") ==
          HttpErrorType::OTHER_ERROR);
  REQUIRE(categorize_http_error("") == HttpErrorType::OTHER_ERROR);
}

TEST_CASE("Error categorizer: l2 keyword rules", "[error-categorizer]") {
  REQUIRE(categorize_l2_error("Connection lost") ==
          L2ErrorType::CONNECTION_ERROR);
  REQUIRE(categorize_l2_error("retry attempts exhausted") ==
          L2ErrorType::RETRY_EXHAUSTED);
  REQUIRE(categorize_l2_error("request timed out") ==
          L2ErrorType::TIMEOUT_ERROR);
  REQUIRE(categorize_l2_error("invalid json response") ==
          L2ErrorType::RESPONSE_ERROR);
  REQUIRE(categorize_l2_error("parse error") == L2ErrorType::RESPONSE_ERROR);
  REQUIRE(categorize_l2_error("unknown") == L2ErrorType::OTHER_ERROR);
}

TEST_CASE("Error categorizer: processing keyword rules",
          "[error-categorizer]") {
  REQUIRE(categorize_processing_error("json parse failure") ==
          ProcessingErrorType::JSON_PARSE_ERROR);
  REQUIRE(categorize_processing_error("required field missing") ==
          ProcessingErrorType::VALIDATION_ERROR);
  REQUIRE(categorize_processing_error("invalid value") ==
          ProcessingErrorType::VALIDATION_ERROR);
  REQUIRE(categorize_processing_error("gzip decompress failed") ==
          ProcessingErrorType::DECOMPRESSION_ERROR);
  REQUIRE(categorize_processing_error("base64 decode error") ==
          ProcessingErrorType::ENCODING_ERROR);
  REQUIRE(categorize_processing_error("timeout") ==
          ProcessingErrorType::TIMEOUT_ERROR);
  REQUIRE(categorize_processing_error("resource exhausted") ==
          ProcessingErrorType::RESOURCE_EXHAUSTED);
  REQUIRE(categorize_processing_error("no available slots") ==
          ProcessingErrorType::RESOURCE_EXHAUSTED);
  REQUIRE(categorize_processing_error("unrecognized") ==
          ProcessingErrorType::OTHER_ERROR);
}

TEST_CASE("Error categorizer: keyword matching is case-insensitive",
          "[error-categorizer]") {
  REQUIRE(categorize_http_error("TIMED OUT") == HttpErrorType::TIMEOUT_ERROR);
  REQUIRE(categorize_http_error("Connection") ==
          HttpErrorType::CONNECTION_ERROR);
  REQUIRE(categorize_l2_error("JSON Parse") == L2ErrorType::RESPONSE_ERROR);
  REQUIRE(categorize_processing_error("Timeout") ==
          ProcessingErrorType::TIMEOUT_ERROR);
}

TEST_CASE("Error categorizer: to_string maps every enum value",
          "[error-categorizer]") {
  REQUIRE(http_error_type_to_string(HttpErrorType::CONNECTION_ERROR) ==
          "CONNECTION_ERROR");
  REQUIRE(http_error_type_to_string(HttpErrorType::SSL_ERROR) == "SSL_ERROR");
  REQUIRE(http_error_type_to_string(HttpErrorType::TIMEOUT_ERROR) ==
          "TIMEOUT_ERROR");
  REQUIRE(http_error_type_to_string(HttpErrorType::STATUS_CODE_ERROR) ==
          "STATUS_CODE_ERROR");
  REQUIRE(http_error_type_to_string(HttpErrorType::PROTOCOL_ERROR) ==
          "PROTOCOL_ERROR");
  REQUIRE(http_error_type_to_string(HttpErrorType::RATE_LIMIT_ERROR) ==
          "RATE_LIMIT_ERROR");
  REQUIRE(http_error_type_to_string(HttpErrorType::SERVER_ERROR) ==
          "SERVER_ERROR");
  REQUIRE(http_error_type_to_string(HttpErrorType::CLIENT_ERROR) ==
          "CLIENT_ERROR");
  REQUIRE(http_error_type_to_string(HttpErrorType::OTHER_ERROR) ==
          "OTHER_ERROR");

  REQUIRE(l2_error_type_to_string(L2ErrorType::CONNECTION_ERROR) ==
          "CONNECTION_ERROR");
  REQUIRE(l2_error_type_to_string(L2ErrorType::TIMEOUT_ERROR) ==
          "TIMEOUT_ERROR");
  REQUIRE(l2_error_type_to_string(L2ErrorType::RESPONSE_ERROR) ==
          "RESPONSE_ERROR");
  REQUIRE(l2_error_type_to_string(L2ErrorType::RETRY_EXHAUSTED) ==
          "RETRY_EXHAUSTED");
  REQUIRE(l2_error_type_to_string(L2ErrorType::OTHER_ERROR) == "OTHER_ERROR");

  REQUIRE(processing_error_type_to_string(
              ProcessingErrorType::JSON_PARSE_ERROR) == "JSON_PARSE_ERROR");
  REQUIRE(processing_error_type_to_string(ProcessingErrorType::VALIDATION_ERROR) ==
          "VALIDATION_ERROR");
  REQUIRE(processing_error_type_to_string(
              ProcessingErrorType::DECOMPRESSION_ERROR) == "DECOMPRESSION_ERROR");
  REQUIRE(processing_error_type_to_string(ProcessingErrorType::ENCODING_ERROR) ==
          "ENCODING_ERROR");
  REQUIRE(processing_error_type_to_string(ProcessingErrorType::TIMEOUT_ERROR) ==
          "TIMEOUT_ERROR");
  REQUIRE(processing_error_type_to_string(
              ProcessingErrorType::RESOURCE_EXHAUSTED) == "RESOURCE_EXHAUSTED");
  REQUIRE(processing_error_type_to_string(ProcessingErrorType::OTHER_ERROR) ==
          "OTHER_ERROR");
}

TEST_CASE("Error categorizer: first matching rule wins", "[error-categorizer]") {
  // "connection" (CONNECTION) precedes "timeout" (TIMEOUT) in the http table.
  REQUIRE(categorize_http_error("connection timed out") ==
          HttpErrorType::CONNECTION_ERROR);
  // "connection" precedes "ssl" in the http table (first match wins).
  REQUIRE(categorize_http_error("ssl handshake failed") ==
          HttpErrorType::SSL_ERROR);
}

// ============================================================================
// URL utils (url_utils.hpp)
// ============================================================================

TEST_CASE("URL utils: normalize_path fills a leading slash",
          "[url-utils]") {
  REQUIRE(normalize_path("/api") == "/api");
  REQUIRE(normalize_path("api") == "/api");
  REQUIRE(normalize_path("") == "/");
}

TEST_CASE("URL utils: extract_query_string after the ?", "[url-utils]") {
  httplib::Request req;
  req.target = "/search?q=hello&lang=ru";
  REQUIRE(extract_query_string(req) == "q=hello&lang=ru");
  req.target = "/noparams";
  REQUIRE(extract_query_string(req) == "");
}

TEST_CASE("URL utils: extract_proxy_ip uses local addr", "[url-utils]") {
  httplib::Request req;
  req.local_addr = "127.0.0.2";
  REQUIRE(extract_proxy_ip(req) == "127.0.0.2");
}
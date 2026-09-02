// Proxy-core unit tests: /v1/sql routing contract (path parsing and the
// action/method decision) and the pure DB gateway helpers. Separate TU from
// test_components.cpp; both run from the Docker build test stage.
#include "db_gateway_routing.hpp"
#include "db_query_utils.hpp"
#include "json_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>

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
#ifndef DB_QUERY_UTILS_HPP
#define DB_QUERY_UTILS_HPP

#include "json_utils.hpp"
#include <cctype>
#include <expected>
#include <string>

// Pure helpers of the HTTP DB Gateway shared by the proxy and the worker.
// Header-only and free of ODPI-C so the unit tests can exercise them without
// the Oracle client installed.

// Strips SQL comments (-- to end of line and /* ... */) from a statement.
// Used by is_read_only_sql() so a comment before the first keyword cannot be
// used to smuggle a non-SELECT statement past the read-only check.
inline std::string strip_sql_comments(const std::string &sql) {
  std::string out;
  out.reserve(sql.size());
  const size_t n = sql.size();
  size_t i = 0;
  while (i < n) {
    if (i + 1 < n && sql[i] == '-' && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (i + 1 < n && sql[i] == '/' && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) {
        ++i;
      }
      i = std::min(i + 2, n);
      continue;
    }
    out.push_back(sql[i]);
    ++i;
  }
  return out;
}

// Reads the next whitespace-delimited word starting at pos, lower-cased.
inline std::string next_word_lower(const std::string &s, size_t &pos) {
  const size_t n = s.size();
  while (pos < n && std::isspace(static_cast<unsigned char>(s[pos]))) {
    ++pos;
  }
  const size_t start = pos;
  while (pos < n &&
         !std::isspace(static_cast<unsigned char>(s[pos]))) {
    ++pos;
  }
  std::string word = s.substr(start, pos - start);
  for (char &c : word) {
    c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
  }
  return word;
}

// True when the statement is a read-only query: the first meaningful keyword
// (after stripping comments and leading parentheses) is SELECT or WITH.
inline bool is_read_only_sql(const std::string &sql) {
  const std::string stripped = strip_sql_comments(sql);
  size_t pos = 0;
  while (pos < stripped.size() && stripped[pos] == '(') {
    ++pos;
  }
  const std::string first = next_word_lower(stripped, pos);
  return first == "select" || first == "with";
}

// Parsed DbQueryContract request received by the worker over NATS.
struct DbQueryRequest {
  std::string m_type; // "query" or "ping"
  std::string m_request_id;
  std::string m_db;
  std::string m_sql;
  // Bind parameters: object of name -> scalar (null/bool/int/double/string).
  json m_params = json::object();
  // -1 means "use the configured default".
  int m_timeout_ms = -1;
  int m_max_rows = -1;
};

// Validates a DbQueryContract request and fills the struct. Returns an error
// message on the first violation.
inline std::expected<DbQueryRequest, std::string>
parse_db_query_request(const json &j) {
  if (!j.is_object()) {
    return std::unexpected("request must be a JSON object");
  }
  DbQueryRequest req;
  req.m_type = JsonUtils::safe_get_string(j, DbQueryContract::kType);
  if (req.m_type != DbQueryContract::kTypeQuery &&
      req.m_type != DbQueryContract::kTypePing) {
    return std::unexpected("type must be \"query\" or \"ping\"");
  }
  req.m_request_id =
      JsonUtils::safe_get_string(j, DbQueryContract::kRequestId);
  req.m_db = JsonUtils::safe_get_string(j, DbQueryContract::kDb);
  req.m_sql = JsonUtils::safe_get_string(j, DbQueryContract::kSql);

  if (req.m_type == DbQueryContract::kTypeQuery) {
    if (req.m_sql.empty()) {
      return std::unexpected("sql must be a non-empty string");
    }
    if (!is_read_only_sql(req.m_sql)) {
      return std::unexpected(
          "only read-only queries are allowed (must start with SELECT or WITH)");
    }
  }

  if (j.contains(DbQueryContract::kParams)) {
    const json &params = j[DbQueryContract::kParams];
    if (params.is_null()) {
      req.m_params = json::object();
    } else if (params.is_object()) {
      for (const auto &[name, value] : params.items()) {
        if (!(value.is_null() || value.is_boolean() || value.is_number() ||
              value.is_string())) {
          return std::unexpected(
              "params must contain only scalar values (null/bool/number/"
              "string)");
        }
        req.m_params[name] = value;
      }
    } else {
      return std::unexpected("params must be an object");
    }
  }

  req.m_timeout_ms = JsonUtils::safe_get_int(j, DbQueryContract::kTimeoutMs, -1);
  if (req.m_timeout_ms < -1 || req.m_timeout_ms == 0) {
    return std::unexpected("timeout_ms must be a positive number");
  }
  req.m_max_rows = JsonUtils::safe_get_int(j, DbQueryContract::kMaxRows, -1);
  if (req.m_max_rows < -1 || req.m_max_rows == 0) {
    return std::unexpected("max_rows must be a positive number");
  }
  return req;
}

// Builds the DbResponseContract error body shared by the proxy and the worker.
inline json make_db_error_body(int http_status, const std::string &code,
                               const std::string &message) {
  (void)http_status;
  return json{
      {DbResponseContract::kStatus, DbResponseContract::kStatusError},
      {DbResponseContract::kError,
       json{{DbResponseContract::kCode, code},
            {DbResponseContract::kMessage, message}}}};
}

// Builds the DbResponseContract success body of a query execution.
inline json make_db_query_response(const std::string &db,
                                   const json &columns, const json &rows,
                                   size_t row_count, bool truncated,
                                   uint64_t duration_ms) {
  return json{
      {DbResponseContract::kStatus, DbResponseContract::kStatusOk},
      {DbResponseContract::kDb, db},
      {DbResponseContract::kColumns, columns},
      {DbResponseContract::kRows, rows},
      {DbResponseContract::kRowCount, row_count},
      {DbResponseContract::kTruncated, truncated},
      {DbResponseContract::kDurationMs, duration_ms}};
}

// Builds the DbResponseContract success body of a ping.
inline json make_db_ping_response(const std::string &db,
                                  uint64_t latency_ms) {
  return json{{DbResponseContract::kStatus, DbResponseContract::kStatusOk},
              {DbResponseContract::kDb, db},
              {DbResponseContract::kLatencyMs, latency_ms}};
}

#endif // DB_QUERY_UTILS_HPP

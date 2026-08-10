#include "db_query_executor_postgres.hpp"
#include "db_query_utils.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include <base64.hpp>
#include <libpq-fe.h>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
// PostgreSQL built-in type OIDs used for JSON value conversion and column
// metadata. Taken from pg_type.h of the stock server.
constexpr Oid kPgBoolOid = 16;
constexpr Oid kPgByteaOid = 17;
constexpr Oid kPgCharOid = 18;
constexpr Oid kPgNameOid = 19;
constexpr Oid kPgInt8Oid = 20;
constexpr Oid kPgInt2Oid = 21;
constexpr Oid kPgInt4Oid = 23;
constexpr Oid kPgTextOid = 25;
constexpr Oid kPgOidOid = 26;
constexpr Oid kPgFloat4Oid = 700;
constexpr Oid kPgFloat8Oid = 701;
constexpr Oid kPgBpcharOid = 1042;
constexpr Oid kPgVarcharOid = 1043;
constexpr Oid kPgDateOid = 1082;
constexpr Oid kPgTimeOid = 1083;
constexpr Oid kPgTimestampOid = 1114;
constexpr Oid kPgTimestamptzOid = 1184;
constexpr Oid kPgTimetzOid = 1266;
constexpr Oid kPgNumericOid = 1700;
constexpr Oid kPgJsonOid = 114;
constexpr Oid kPgJsonbOid = 3802;
constexpr Oid kPgUuidOid = 2950;

uint64_t pg_steady_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string pg_type_name(Oid oid) {
  switch (oid) {
  case kPgBoolOid:
    return "BOOLEAN";
  case kPgByteaOid:
    return "BYTEA";
  case kPgCharOid:
    return "CHAR";
  case kPgNameOid:
    return "NAME";
  case kPgInt2Oid:
    return "SMALLINT";
  case kPgInt4Oid:
    return "INTEGER";
  case kPgInt8Oid:
    return "BIGINT";
  case kPgTextOid:
    return "TEXT";
  case kPgOidOid:
    return "OID";
  case kPgFloat4Oid:
    return "REAL";
  case kPgFloat8Oid:
    return "DOUBLE PRECISION";
  case kPgBpcharOid:
    return "BPCHAR";
  case kPgVarcharOid:
    return "VARCHAR";
  case kPgDateOid:
    return "DATE";
  case kPgTimeOid:
    return "TIME";
  case kPgTimestampOid:
    return "TIMESTAMP";
  case kPgTimestamptzOid:
    return "TIMESTAMPTZ";
  case kPgTimetzOid:
    return "TIMETZ";
  case kPgNumericOid:
    return "NUMERIC";
  case kPgJsonOid:
    return "JSON";
  case kPgJsonbOid:
    return "JSONB";
  case kPgUuidOid:
    return "UUID";
  default:
    return std::format("UNKNOWN({})", static_cast<uint32_t>(oid));
  }
}

// Decodes PostgreSQL hex bytea output ("\x....") into raw bytes.
std::string hex_to_bytes(const std::string &hex) {
  const size_t start = (!hex.empty() && hex[0] == '\\' && hex.size() > 1 &&
                        hex[1] == 'x')
                           ? 2
                           : 0;
  const auto nibble = [](char c) -> unsigned char {
    if (c >= '0' && c <= '9') {
      return static_cast<unsigned char>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
      return static_cast<unsigned char>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
      return static_cast<unsigned char>(c - 'A' + 10);
    }
    return 0;
  };
  std::string out;
  out.reserve((hex.size() - start) / 2);
  for (size_t i = start; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  }
  return out;
}

// Converts a text-format value to the JSON value matching the column type.
json pg_value(Oid oid, const char *text, int len) {
  const std::string s(text, len);
  switch (oid) {
  case kPgBoolOid:
    return len > 0 && text[0] == 't';
  case kPgInt2Oid:
  case kPgInt4Oid:
  case kPgInt8Oid:
  case kPgOidOid:
    try {
      return std::stoll(s);
    } catch (const std::exception &) {
      return s;
    }
  case kPgFloat4Oid:
  case kPgFloat8Oid:
  case kPgNumericOid:
    try {
      return std::stod(s);
    } catch (const std::exception &) {
      return s;
    }
  case kPgByteaOid:
    return base64::to_base64(hex_to_bytes(s));
  default:
    return s;
  }
}
} // namespace

struct PostgresQueryExecutor::Impl {
  const DbConfig m_db;
  // Idle pooled connections. Total connections never exceed m_db.m_pool_max:
  // acquire() only creates when below the limit and every connection is
  // returned to the pool on release.
  std::vector<PGconn *> m_idle;
  size_t m_total = 0;

  explicit Impl(DbConfig db) : m_db(std::move(db)) {}

  ~Impl() {
    for (PGconn *conn : m_idle) {
      PQfinish(conn);
    }
    m_idle.clear();
  }

  PGconn *create_conn() const {
    const std::string port = std::to_string(m_db.m_port);
    const std::vector<const char *> keywords = {
        "host", "port", "dbname", "user", "password", "connect_timeout",
        nullptr};
    const std::vector<const char *> values = {
        m_db.m_host.c_str(),  port.c_str(),
        m_db.m_database.c_str(), m_db.m_user.c_str(),
        m_db.m_password.c_str(), "10",
        nullptr};
    return PQconnectdbParams(keywords.data(), values.data(), 0);
  }

  std::optional<PGconn *> acquire_conn() {
    if (!m_idle.empty()) {
      PGconn *conn = m_idle.back();
      m_idle.pop_back();
      return conn;
    }
    if (m_total >= static_cast<size_t>(m_db.m_pool_max)) {
      return std::nullopt;
    }
    PGconn *conn = create_conn();
    if (!conn) {
      return std::nullopt;
    }
    ++m_total;
    if (PQstatus(conn) != CONNECTION_OK) {
      Logger::warn("DB executor '{}': connect failed: {}", m_db.m_name,
                   trim_copy(PQerrorMessage(conn)));
      PQfinish(conn);
      --m_total;
      return std::nullopt;
    }
    return conn;
  }

  void release_conn(PGconn *conn) {
    if (!conn) {
      return;
    }
    if (PQstatus(conn) != CONNECTION_OK) {
      PQfinish(conn);
      if (m_total > 0) {
        --m_total;
      }
      return;
    }
    m_idle.push_back(conn);
  }

  static std::string trim_copy(const char *text) {
    if (!text) {
      return "";
    }
    std::string s(text);
    while (!s.empty() &&
           (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
      s.pop_back();
    }
    return s;
  }
};

PostgresQueryExecutor::PostgresQueryExecutor(DbConfig db)
    : m_impl(std::make_unique<Impl>(std::move(db))) {}

PostgresQueryExecutor::~PostgresQueryExecutor() = default;

bool PostgresQueryExecutor::init() {
  // Validate credentials eagerly: libpq only reports bad dbname/user on the
  // first connection, so probe once here (like ODPI pool creation does).
  auto maybe_conn = m_impl->acquire_conn();
  if (!maybe_conn) {
    Logger::error("DB executor '{}': failed to connect to {}:{}",
                  m_impl->m_db.m_name, m_impl->m_db.m_host,
                  m_impl->m_db.m_port);
    return false;
  }
  m_impl->release_conn(*maybe_conn);
  Logger::info("DB executor '{}': pool ready ({}..{} sessions, connect "
               "{}:{} db={})",
               m_impl->m_db.m_name, m_impl->m_db.m_pool_min,
               m_impl->m_db.m_pool_max, m_impl->m_db.m_host,
               m_impl->m_db.m_port, m_impl->m_db.m_database);
  return true;
}

int PostgresQueryExecutor::default_timeout_ms() const {
  return m_impl->m_db.m_query_timeout_ms;
}

int PostgresQueryExecutor::default_max_rows() const {
  return m_impl->m_db.m_max_rows;
}

const std::string &PostgresQueryExecutor::db_name() const {
  return m_impl->m_db.m_name;
}

json PostgresQueryExecutor::execute_query(const std::string &sql,
                                          const json &params, int timeout_ms,
                                          int max_rows, int &status_code) {
  status_code = 200;
  const uint64_t start_ms = pg_steady_ms();

  auto maybe_conn = m_impl->acquire_conn();
  if (!maybe_conn) {
    status_code = 503;
    return make_db_error_body(status_code, "DB_UNAVAILABLE",
                              "Failed to acquire a database connection");
  }
  PGconn *conn = *maybe_conn;

  const int effective_timeout =
      timeout_ms > 0 ? timeout_ms : m_impl->m_db.m_query_timeout_ms;
  PGresult *set_res = PQexec(
      conn, std::format("SET statement_timeout = {}", effective_timeout)
                .c_str());
  const bool set_ok = PQresultStatus(set_res) == PGRES_COMMAND_OK;
  std::string set_error = Impl::trim_copy(PQerrorMessage(conn));
  if (set_res) {
    PQclear(set_res);
  }
  if (!set_ok) {
    m_impl->release_conn(conn);
    status_code = 422;
    return make_db_error_body(status_code, "SQL_ERROR",
                              "Failed to set statement timeout: " + set_error);
  }

  // Bind parameters as positional $1..$N. Params are passed in text format
  // (the server casts to the inferred placeholder type); NULL is signalled by
  // a null value pointer.
  std::vector<std::string> text_values;
  std::vector<const char *> param_values;
  std::vector<int> param_lengths;
  text_values.reserve(params.size());
  param_values.reserve(params.size());
  param_lengths.reserve(params.size());
  for (const auto &[name, value] : params.items()) {
    (void)name;
    if (value.is_null()) {
      text_values.emplace_back();
      param_values.push_back(nullptr);
      param_lengths.push_back(0);
    } else if (value.is_boolean()) {
      text_values.push_back(value.get<bool>() ? "t" : "f");
      param_values.push_back(text_values.back().c_str());
      param_lengths.push_back(static_cast<int>(text_values.back().size()));
    } else if (value.is_number_integer()) {
      text_values.push_back(std::to_string(value.get<int64_t>()));
      param_values.push_back(text_values.back().c_str());
      param_lengths.push_back(static_cast<int>(text_values.back().size()));
    } else if (value.is_number_float()) {
      text_values.push_back(std::to_string(value.get<double>()));
      param_values.push_back(text_values.back().c_str());
      param_lengths.push_back(static_cast<int>(text_values.back().size()));
    } else if (value.is_string()) {
      text_values.push_back(value.get<std::string>());
      param_values.push_back(text_values.back().c_str());
      param_lengths.push_back(static_cast<int>(text_values.back().size()));
    }
  }

  PGresult *res = PQexecParams(
      conn, sql.c_str(), static_cast<int>(param_values.size()), nullptr,
      param_values.data(), param_lengths.data(), nullptr, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    const std::string message = Impl::trim_copy(PQresultErrorMessage(res));
    PQclear(res);
    m_impl->release_conn(conn);
    status_code = 422;
    return make_db_error_body(status_code, "SQL_ERROR", message);
  }

  const int num_fields = PQnfields(res);
  const int num_tuples = PQntuples(res);
  if (num_fields == 0) {
    PQclear(res);
    m_impl->release_conn(conn);
    status_code = 422;
    return make_db_error_body(status_code, "SQL_ERROR",
                              "Statement returned no result set");
  }

  json rows = json::array();
  bool truncated = false;
  for (int r = 0; r < num_tuples; ++r) {
    if (rows.size() >= static_cast<size_t>(max_rows)) {
      truncated = true;
      break;
    }
    json row = json::array();
    for (int c = 0; c < num_fields; ++c) {
      if (PQgetisnull(res, r, c)) {
        row.push_back(nullptr);
        continue;
      }
      const Oid oid = PQftype(res, c);
      row.push_back(pg_value(oid, PQgetvalue(res, r, c), PQgetlength(res, r, c)));
    }
    rows.push_back(std::move(row));
  }

  json columns_json = json::array();
  for (int c = 0; c < num_fields; ++c) {
    columns_json.push_back(json{
        {DbResponseContract::kName, PQfname(res, c)},
        {DbResponseContract::kType, pg_type_name(PQftype(res, c))}});
  }
  PQclear(res);
  m_impl->release_conn(conn);

  const uint64_t end_ms = pg_steady_ms();
  return make_db_query_response(m_impl->m_db.m_name, columns_json, rows,
                                rows.size(), truncated, end_ms - start_ms);
}

bool PostgresQueryExecutor::ping(int timeout_ms) {
  (void)timeout_ms;
  auto maybe_conn = m_impl->acquire_conn();
  if (!maybe_conn) {
    return false;
  }
  PGconn *conn = *maybe_conn;
  PGresult *res = PQexec(conn, "SELECT 1");
  const bool ok = PQresultStatus(res) == PGRES_TUPLES_OK;
  if (!ok) {
    Logger::warn("DB executor '{}': ping failed: {}", m_impl->m_db.m_name,
                 Impl::trim_copy(PQerrorMessage(conn)));
  }
  PQclear(res);
  m_impl->release_conn(conn);
  return ok;
}

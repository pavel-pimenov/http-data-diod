#include "db_query_executor_postgres.hpp"
#include "config.hpp"
#include "db_query_executor_base.hpp"
#include "db_query_utils.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "pg_value_utils.hpp"
#include "string_utils.hpp"
#include "time_utils.hpp"
#include <libpq-fe.h>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct PostgresQueryExecutor::Impl {
  const DbConfig &m_db;
  DbExecutorBase *m_owner = nullptr;
  // Idle pooled connections. Total connections never exceed m_db.m_pool_max:
  // acquire() only creates when below the limit and every connection is
  // returned to the pool on release. Guarded by m_mutex because
  // DbQueryHandler is accessed from the worker thread-pool (concurrent NATS
  // DB requests).
  std::vector<PGconn *> m_idle;
  size_t m_total = 0;
  mutable std::mutex m_mutex;

  explicit Impl(const DbConfig &db, DbExecutorBase *owner)
      : m_db(db), m_owner(owner) {}

  ~Impl() {
    const std::scoped_lock lock(m_mutex);
    for (PGconn *conn : m_idle) {
      PQfinish(conn);
    }
    m_idle.clear();
  }

  void update_pool_gauges_locked() const {
    const auto idle = static_cast<double>(m_idle.size());
    const auto active =
        static_cast<double>(m_total >= m_idle.size() ? m_total - m_idle.size()
                                                     : 0);
    m_owner->set_db_pool_gauges(idle, active);
  }

  void update_pool_gauges() const {
    const std::scoped_lock lock(m_mutex);
    update_pool_gauges_locked();
  }

  PGconn *create_conn() const {
    const auto port = std::to_string(m_db.m_port);
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
    {
      const std::scoped_lock lock(m_mutex);
      if (!m_idle.empty()) {
        PGconn *conn = m_idle.back();
        m_idle.pop_back();
        update_pool_gauges_locked();
        return conn;
      }
      if (m_total >= static_cast<size_t>(m_db.m_pool_max)) {
        return std::nullopt;
      }
      // reserve slot
      ++m_total;
      update_pool_gauges_locked();
    }
    PGconn *conn = create_conn();
    if (!conn) {
      const std::scoped_lock lock(m_mutex);
      if (m_total > 0) {
        --m_total;
      }
      update_pool_gauges_locked();
      return std::nullopt;
    }
    if (PQstatus(conn) != CONNECTION_OK) {
      Logger::warn("DB executor '{}': connect failed: {}", m_db.m_name,
                   trim_copy(PQerrorMessage(conn)));
      PQfinish(conn);
      const std::scoped_lock lock(m_mutex);
      if (m_total > 0) {
        --m_total;
      }
      update_pool_gauges_locked();
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
      const std::scoped_lock lock(m_mutex);
      if (m_total > 0) {
        --m_total;
      }
      update_pool_gauges_locked();
      return;
    }
    const std::scoped_lock lock(m_mutex);
    m_idle.push_back(conn);
    update_pool_gauges_locked();
  }

  static std::string trim_copy(const char *text) {
    return text == nullptr ? std::string() : trim_trailing(text, "\r\n ");
  }

  // Applies the per-query statement_timeout; returns "" on success, otherwise
  // the connection's last error text (trimmed).
  static std::string set_statement_timeout(PGconn *conn, int timeout_ms) {
    PGresult *set_res = PQexec(
        conn, std::format("SET statement_timeout = {}", timeout_ms).c_str());
    const bool set_ok = set_res && PQresultStatus(set_res) == PGRES_COMMAND_OK;
    if (set_res) {
      PQclear(set_res);
    }
    if (!set_ok) {
      return trim_copy(PQerrorMessage(conn));
    }
    return "";
  }
};

PostgresQueryExecutor::PostgresQueryExecutor(DbConfig db)
    : DbExecutorBase(std::move(db)),
      m_impl(std::make_unique<Impl>(m_db, this)) {}

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

void PostgresQueryExecutor::refresh_pool_gauges() {
  if (m_impl) {
    m_impl->update_pool_gauges();
  }
}

json PostgresQueryExecutor::execute_query(const std::string &sql,
                                          const json &params, int timeout_ms,
                                          int max_rows, int &status_code) {
  status_code = 200;
  const uint64_t start_ms = TimeUtils::steady_ms();

  auto maybe_conn = m_impl->acquire_conn();
  if (!maybe_conn) {
    return make_db_unavailable(status_code);
  }
  PGconn *conn = *maybe_conn;

  const int effective_timeout =
      resolve_positive_or(timeout_ms, m_impl->m_db.m_query_timeout_ms);
  const std::string timeout_error =
      Impl::set_statement_timeout(conn, effective_timeout);
  if (!timeout_error.empty()) {
    m_impl->release_conn(conn);
    return make_db_sql_error(status_code,
                             "Failed to set statement timeout: " +
                                 timeout_error);
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
      text_values.emplace_back(value.get<bool>() ? "t" : "f");
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
    const auto message = Impl::trim_copy(PQresultErrorMessage(res));
    PQclear(res);
    m_impl->release_conn(conn);
    return make_db_sql_error(status_code, message);
  }

  const int num_fields = PQnfields(res);
  const int num_tuples = PQntuples(res);
  if (num_fields == 0) {
    PQclear(res);
    m_impl->release_conn(conn);
    return make_db_sql_error(status_code, "Statement returned no result set");
  }

  DbRowCollector rows(static_cast<size_t>(max_rows));
  for (int r = 0; r < num_tuples; ++r) {
    json row = json::array();
    for (int c = 0; c < num_fields; ++c) {
      if (PQgetisnull(res, r, c)) {
        row.push_back(nullptr);
        continue;
      }
      const auto oid = static_cast<uint32_t>(PQftype(res, c));
      row.push_back(pg_value_utils::pg_value(
          oid, PQgetvalue(res, r, c), PQgetlength(res, r, c)));
    }
    if (!rows.try_add(std::move(row))) {
      break;
    }
  }

  std::vector<std::pair<std::string, std::string>> name_type;
  name_type.reserve(num_fields);
  for (int c = 0; c < num_fields; ++c) {
    name_type.emplace_back(
        PQfname(res, c),
        pg_value_utils::pg_type_name(static_cast<uint32_t>(PQftype(res, c))));
  }
  const json columns_json = make_db_columns_json(name_type);
  PQclear(res);
  m_impl->release_conn(conn);
  return build_query_response(columns_json, rows, start_ms);
}

bool PostgresQueryExecutor::ping(int timeout_ms) {
  auto maybe_conn = m_impl->acquire_conn();
  if (!maybe_conn) {
    return false;
  }
  PGconn *conn = *maybe_conn;
  const int effective_timeout =
      resolve_positive_or(timeout_ms, m_impl->m_db.m_query_timeout_ms);
  // Apply statement_timeout even to ping so an overloaded PG does not block a
  // worker thread indefinitely.
  const std::string timeout_error =
      Impl::set_statement_timeout(conn, effective_timeout);
  if (!timeout_error.empty()) {
    Logger::warn("DB executor '{}': ping failed to set statement_timeout",
                 m_impl->m_db.m_name);
    m_impl->release_conn(conn);
    return false;
  }
  PGresult *res = PQexec(conn, "SELECT 1");
  const bool ok = res && PQresultStatus(res) == PGRES_TUPLES_OK;
  if (!ok) {
    Logger::warn("DB executor '{}': ping failed: {}", m_impl->m_db.m_name,
                 res ? Impl::trim_copy(PQresultErrorMessage(res))
                     : Impl::trim_copy(PQerrorMessage(conn)));
  }
  if (res) {
    PQclear(res);
  }
  m_impl->release_conn(conn);
  return ok;
}
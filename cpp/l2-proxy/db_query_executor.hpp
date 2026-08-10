#ifndef DB_QUERY_EXECUTOR_HPP
#define DB_QUERY_EXECUTOR_HPP

#include "config.hpp"
#include "nlohmann/json.hpp"
#include <memory>
#include <string>

using json = nlohmann::json;

// Driver-agnostic interface of the HTTP DB Gateway executor. One executor is
// created per configured database by create_db_query_executor() based on
// DbConfig::m_driver ("oracle" uses ODPI-C, "postgres" uses libpq). Executors
// are owned by the DB gateway handler and are not shared between threads: all
// pool access happens from one thread.
class DbQueryExecutor {
public:
  virtual ~DbQueryExecutor() = default;

  // Creates the driver context and pool. Must be called (and return true)
  // before execute_query()/ping().
  virtual bool init() = 0;

  // Configured defaults applied when a request does not override them.
  [[nodiscard]] virtual int default_timeout_ms() const = 0;
  [[nodiscard]] virtual int default_max_rows() const = 0;
  [[nodiscard]] virtual const std::string &db_name() const = 0;

  // Executes a read-only query. On success status_code is set to 200 and the
  // returned body is a DbResponseContract query response; on failure
  // status_code carries the mapped HTTP status and the body is an
  // ErrorResponse object.
  virtual json execute_query(const std::string &sql, const json &params,
                             int timeout_ms, int max_rows, int &status_code) = 0;

  // Lightweight connectivity check ("SELECT 1"). Returns true when a
  // connection could be acquired and the probe succeeded.
  virtual bool ping(int timeout_ms) = 0;

protected:
  DbQueryExecutor() = default;
};

// Creates the executor matching DbConfig::m_driver ("oracle" | "postgres").
// Returns nullptr for an unknown driver.
std::unique_ptr<DbQueryExecutor> create_db_query_executor(const DbConfig &db);

#endif // DB_QUERY_EXECUTOR_HPP

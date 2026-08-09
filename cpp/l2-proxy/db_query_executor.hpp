#ifndef DB_QUERY_EXECUTOR_HPP
#define DB_QUERY_EXECUTOR_HPP

#include "config.hpp"
#include "nlohmann/json.hpp"
#include <memory>
#include <string>

using json = nlohmann::json;

// Executes read-only SQL against a single database using ODPI-C and a
// connection pool. Instances are owned by the DB gateway handler and are not
// shared between threads: all pool access happens from one thread.
class DbQueryExecutor {
public:
  explicit DbQueryExecutor(DbConfig db);
  ~DbQueryExecutor();
  DbQueryExecutor(const DbQueryExecutor &) = delete;
  DbQueryExecutor &operator=(const DbQueryExecutor &) = delete;

  // Creates the ODPI-C context and pool. Must be called (and return true)
  // before execute_query()/ping().
  bool init();

  // Configured defaults applied when a request does not override them.
  [[nodiscard]] int default_timeout_ms() const;
  [[nodiscard]] int default_max_rows() const;
  [[nodiscard]] const std::string &db_name() const;

  // Executes a read-only query. On success status_code is set to 200 and the
  // returned body is a DbResponseContract query response; on failure
  // status_code carries the mapped HTTP status and the body is an
  // ErrorResponse object.
  json execute_query(const std::string &sql, const json &params, int timeout_ms,
                     int max_rows, int &status_code);

  // Lightweight connectivity check ("SELECT 1 FROM DUAL"). Returns true when a
  // connection could be acquired and the probe succeeded.
  bool ping(int timeout_ms);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

#endif // DB_QUERY_EXECUTOR_HPP

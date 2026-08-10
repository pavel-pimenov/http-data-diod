#ifndef DB_QUERY_EXECUTOR_ORACLE_HPP
#define DB_QUERY_EXECUTOR_ORACLE_HPP

#include "config.hpp"
#include "db_query_executor.hpp"
#include <memory>

// Oracle implementation of DbQueryExecutor built on ODPI-C and a connection
// pool. Instances are owned by the DB gateway handler and are not shared
// between threads: all pool access happens from one thread.
class OracleQueryExecutor final : public DbQueryExecutor {
public:
  explicit OracleQueryExecutor(DbConfig db);
  ~OracleQueryExecutor() override;
  OracleQueryExecutor(const OracleQueryExecutor &) = delete;
  OracleQueryExecutor &operator=(const OracleQueryExecutor &) = delete;

  bool init() override;
  [[nodiscard]] int default_timeout_ms() const override;
  [[nodiscard]] int default_max_rows() const override;
  [[nodiscard]] const std::string &db_name() const override;
  json execute_query(const std::string &sql, const json &params, int timeout_ms,
                     int max_rows, int &status_code) override;
  bool ping(int timeout_ms) override;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

#endif // DB_QUERY_EXECUTOR_ORACLE_HPP

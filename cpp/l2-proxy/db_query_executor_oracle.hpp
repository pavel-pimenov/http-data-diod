#ifndef DB_QUERY_EXECUTOR_ORACLE_HPP
#define DB_QUERY_EXECUTOR_ORACLE_HPP

#include "config.hpp"
#include "db_query_executor_base.hpp"
#include <atomic>
#include <memory>
#include <thread>

struct dpiConn;

// Oracle implementation of DbQueryExecutor built on ODPI-C and a connection
// pool. Instances are owned by the DB gateway handler and are not shared
// between threads: all pool access happens from one thread.
class OracleQueryExecutor final : public DbExecutorBase {
public:
  explicit OracleQueryExecutor(DbConfig db);
  ~OracleQueryExecutor() override;

  bool init() override;
  json execute_query(const std::string &sql, const json &params, int timeout_ms,
                     int max_rows, int &status_code) override;
  bool ping(int timeout_ms) override;

protected:
  void refresh_pool_gauges() override;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  // True once the background init() reported the ODPI pool ready. Requests
  // arrive only after init() returns, so until this flips they are answered
  // with DB_UNAVAILABLE instead of touching a not-yet-created pool.
  std::atomic<bool> m_ready{false};
  // Runs the (potentially long) ODPI pool creation off the worker main loop so
  // an unreachable Oracle host cannot block the worker's NATS subscription.
  std::thread m_init_thread;

  // Returns the pooled connection to the pool and refreshes pool gauges.
  void release_conn(dpiConn *conn);

  // RAII guard returning the acquired connection to the pool on scope exit.
  // Defined once in the .cpp (was previously duplicated in execute_query and
  // ping).
  struct ConnGuard;
};

#endif // DB_QUERY_EXECUTOR_ORACLE_HPP
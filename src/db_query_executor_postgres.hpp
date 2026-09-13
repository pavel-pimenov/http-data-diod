#ifndef DB_QUERY_EXECUTOR_POSTGRES_HPP
#define DB_QUERY_EXECUTOR_POSTGRES_HPP

#include "config.hpp"
#include "db_query_executor_base.hpp"
#include <memory>

// PostgreSQL implementation of DbQueryExecutor built on libpq and a small
// connection pool. Instances are owned by the DB gateway handler and may be
// accessed from multiple worker threads (NATS DB requests run on the thread
// pool), so pool state is guarded by a mutex.
class PostgresQueryExecutor final : public DbExecutorBase {
public:
  explicit PostgresQueryExecutor(DbConfig db);
  ~PostgresQueryExecutor() override;

  bool init() override;
  json execute_query(const std::string &sql, const json &params, int timeout_ms,
                     int max_rows, int &status_code) override;
  bool ping(int timeout_ms) override;

protected:
  void refresh_pool_gauges() override;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

#endif // DB_QUERY_EXECUTOR_POSTGRES_HPP
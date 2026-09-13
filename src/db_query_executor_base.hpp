#ifndef DB_QUERY_EXECUTOR_BASE_HPP
#define DB_QUERY_EXECUTOR_BASE_HPP

#include "config.hpp"
#include "db_query_executor.hpp"
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <string>

// Shared state and overrides of DbQueryExecutor: the database config, the
// configured-default getters and the pool-gauge wiring. Drivers keep their
// driver-specific pool in a pimpl (so the libpq/ODPI headers stay out of the
// public header) and only implement init/execute_query/ping plus the
// driver-specific idle/active pool computation.
class DbExecutorBase : public DbQueryExecutor {
public:
  explicit DbExecutorBase(DbConfig db);
  ~DbExecutorBase() override = default;
  DbExecutorBase(const DbExecutorBase &) = delete;
  DbExecutorBase &operator=(const DbExecutorBase &) = delete;

  [[nodiscard]] int default_timeout_ms() const override;
  [[nodiscard]] int default_max_rows() const override;
  [[nodiscard]] const std::string &db_name() const override;

  void set_pool_metrics(
      prometheus::Family<prometheus::Gauge> *pool_metrics) override;

  // Publishes the per-database idle/active pool gauges. Driver code (including
  // the driver pimpl, which lives outside the class hierarchy) calls this from
  // refresh_pool_gauges() and after each acquire/release.
  void set_db_pool_gauges(double idle, double active);

protected:
  // Refreshes the pool gauges from the driver's current pool state. Called by
  // set_pool_metrics(); must tolerate a not-yet-created driver pool.
  virtual void refresh_pool_gauges() = 0;

  const DbConfig m_db;
  prometheus::Family<prometheus::Gauge> *m_pool_metrics = nullptr;
};

#endif // DB_QUERY_EXECUTOR_BASE_HPP
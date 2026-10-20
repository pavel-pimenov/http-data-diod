#ifndef DB_QUERY_EXECUTOR_BASE_HPP
#define DB_QUERY_EXECUTOR_BASE_HPP

#include "config.hpp"
#include "db_query_executor.hpp"
#include <cstdint>
#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <string>

class DbRowCollector;

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

  void set_error_metrics(
      prometheus::Family<prometheus::Counter> *errors_by_class) override;

  // Counts one failed query under its error class. The class is a short,
  // bounded label (the SQLSTATE class, e.g. "08" for a connection problem);
  // an empty class means "could not classify".
  void count_query_error(const std::string &error_class) const;

  void set_pool_saturation_metrics(
      prometheus::Family<prometheus::Gauge> *pool_capacity,
      prometheus::Family<prometheus::Counter> *pool_exhausted,
      prometheus::Family<prometheus::Histogram> *pool_acquire_duration)
      override;

  // Publishes the per-database idle/active pool gauges. Driver code (including
  // the driver pimpl, which lives outside the class hierarchy) calls this from
  // refresh_pool_gauges() and after each acquire/release.
  void set_db_pool_gauges(double idle, double active);

  prometheus::Family<prometheus::Counter> *m_errors_by_class = nullptr;

  // Saturation side of the pool wiring, called by driver pools on every
  // acquire attempt: the capacity is published once by
  // set_pool_saturation_metrics(), exhaustion and acquire latency per attempt.
  void publish_pool_exhausted();
  void observe_pool_acquire(double seconds);

protected:
  // Refreshes the pool gauges from the driver's current pool state. Called by
  // set_pool_metrics(); must tolerate a not-yet-created driver pool.
  virtual void refresh_pool_gauges() = 0;

  // Assembles the DbResponseContract success body: collects the row count /
  // truncated flag / duration and stamps the database name. Shared by the
  // driver execute_query() tails.
  [[nodiscard]] json build_query_response(const json &columns_json,
                                          DbRowCollector &rows,
                                          uint64_t start_ms) const;

  const DbConfig m_db;
  prometheus::Family<prometheus::Gauge> *m_pool_metrics = nullptr;
  prometheus::Family<prometheus::Gauge> *m_pool_capacity = nullptr;
  prometheus::Family<prometheus::Counter> *m_pool_exhausted = nullptr;
  prometheus::Family<prometheus::Histogram> *m_pool_acquire_duration = nullptr;
};

#endif // DB_QUERY_EXECUTOR_BASE_HPP
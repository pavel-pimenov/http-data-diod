#ifndef DB_QUERY_EXECUTOR_HPP
#define DB_QUERY_EXECUTOR_HPP

#include "config.hpp"
#include "nlohmann/json.hpp"
#include <memory>
#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
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

  // True when the driver is fully initialized and can serve requests.
  // Synchronous drivers are always ready after init(); async ones (Oracle
  // inits the ODPI pool on a background thread) report readiness only once
  // the pool exists.
  [[nodiscard]] virtual bool is_ready() const { return true; }

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

  // Wires the per-database pool gauge family (l2_worker_db_pool_connections)
  // so the executor can publish active/idle connection counts on every
  // acquire/release. nullptr (default) disables the updates.
  virtual void
  set_pool_metrics(prometheus::Family<prometheus::Gauge> *pool_metrics) = 0;

  // Wires the counter of failed queries broken down by error class (the
  // SQLSTATE class for PostgreSQL). Drivers that cannot report a class
  // (Oracle) inherit DbExecutorBase's implementation and simply never publish.
  virtual void set_error_metrics(
      prometheus::Family<prometheus::Counter> *errors_by_class) = 0;

  // Wires the per-database pool saturation families (capacity, exhaustion
  // counter, acquire latency). Drivers whose pool cannot saturate (Oracle)
  // inherit DbExecutorBase's implementation and simply never publish.
  virtual void set_pool_saturation_metrics(
      prometheus::Family<prometheus::Gauge> *pool_capacity,
      prometheus::Family<prometheus::Counter> *pool_exhausted,
      prometheus::Family<prometheus::Histogram> *pool_acquire_duration) = 0;

protected:
  DbQueryExecutor() = default;
};

// Creates the executor matching DbConfig::m_driver ("oracle" | "postgres").
// Returns nullptr for an unknown driver.
std::unique_ptr<DbQueryExecutor> create_db_query_executor(const DbConfig &db);

#endif // DB_QUERY_EXECUTOR_HPP

#ifndef DB_QUERY_HANDLER_HPP
#define DB_QUERY_HANDLER_HPP

#include "config.hpp"
#include "db_query_executor.hpp"
#include "nlohmann/json.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <string>
#include <vector>

using json = nlohmann::json;

// Owns one DbQueryExecutor per configured database and routes parsed
// DbQueryContract requests to them. Used by the worker's NATS DB subscription.
class DbQueryHandler {
public:
  DbQueryHandler() = default;
  ~DbQueryHandler() = default;
  DbQueryHandler(const DbQueryHandler &) = delete;
  DbQueryHandler &operator=(const DbQueryHandler &) = delete;

  // Creates executors for configured databases that do not have one yet (the
  // caller retries while some database is still starting up, e.g. Oracle cold
  // start). Returns false when no executor could be initialized.
  bool init(const std::vector<DbConfig> &databases);

  [[nodiscard]] bool is_enabled() const {
    const std::scoped_lock lock(m_state.m_mutex);
    return !m_state.m_executors.empty();
  }

  // True when every configured database has an initialized executor. The
  // worker retries init() until this holds so a fast-starting database (e.g.
  // PostgreSQL) cannot mask a slow one (Oracle cold start).
  [[nodiscard]] bool all_configured() const {
    const std::scoped_lock lock(m_state.m_mutex);
    return m_state.m_expected_count > 0 &&
           m_state.m_executors.size() >= m_state.m_expected_count;
  }

  // True when every configured executor reports itself fully ready (e.g. the
  // Oracle ODPI pool has finished its background init). Used by the spider to
  // avoid polling databases whose pools are still warming up.
  [[nodiscard]] bool all_ready() const {
    const std::scoped_lock lock(m_state.m_mutex);
    if (m_state.m_executors.empty()) {
      return false;
    }
    for (const auto &[name, executor] : m_state.m_executors) {
      (void)name;
      if (!executor->is_ready()) {
        return false;
      }
    }
    return true;
  }

  // Names of every configured database. Used for the per-database gateway
  // readiness gauge so a slow-starting database is visible before init().
  [[nodiscard]] std::vector<std::string> configured_databases() const;

  // Names of configured databases that have a live executor already.
  [[nodiscard]] std::vector<std::string> ready_databases() const;

  // Executes one DbQueryContract request. Fills status_code with the HTTP
  // status of the DB gateway response and body with its JSON body
  // (success body or ErrorResponse object).
  void handle_request(const json &request, int &status_code, json &body);

  // Executes a query row-by-row via the executor's stream_query, invoking
  // `row_cb` for every row. On success status_code = 200 and columns_out is
  // the column descriptors; on failure status_code carries the mapped HTTP
  // status. Returns the row count fetched. Used by l2-db-spider to write JSON
  // files incrementally without materialising the full result set.
  int stream_request(const json &request, int &status_code, json &columns_out,
                     DbQueryExecutor::RowCallback row_cb);

  // Wires the pool gauge family into every (future) executor created by
  // init(). Safe to call before init(); executors already created keep their
  // current pointer.
  // Wires the counter of failed queries broken down by error class.
  void set_error_metrics(prometheus::Family<prometheus::Counter> *errors_by_class);

  void set_pool_metrics(prometheus::Family<prometheus::Gauge> *pool_metrics);

  // Wires the pool saturation families (capacity, exhaustion counter, acquire
  // latency) into every (future) executor created by init().
  void set_pool_saturation_metrics(
      prometheus::Family<prometheus::Gauge> *pool_capacity,
      prometheus::Family<prometheus::Counter> *pool_exhausted,
      prometheus::Family<prometheus::Histogram> *pool_acquire_duration);

private:
  struct State {
    // Базы (postgres/oracle) — map (rb-tree). Убрали стандартный flat_map: он
    // тянет за собой дополнительные заголовки и раздувает размер исходников;
    // map достаточно для 1-2 баз.
    std::map<std::string, std::unique_ptr<DbQueryExecutor>> m_executors;
    // Serializes init() (runs on the worker main loop) against request dispatch
    // (runs on pool threads) so a slow/blocked executor creation can never race
    // with reads of m_executors.
    mutable std::mutex m_mutex;
    size_t m_expected_count = 0;
    // Insertion-ordered configured database names (mirrors m_expected_count);
    // kept so readiness can be reported for databases that have no executor yet.
    std::vector<std::string> m_configured_names;
  } m_state;
  prometheus::Family<prometheus::Gauge> *m_pool_metrics = nullptr;
  prometheus::Family<prometheus::Counter> *m_errors_by_class = nullptr;
  prometheus::Family<prometheus::Gauge> *m_pool_capacity = nullptr;
  prometheus::Family<prometheus::Counter> *m_pool_exhausted = nullptr;
  prometheus::Family<prometheus::Histogram> *m_pool_acquire_duration = nullptr;
};

#endif // DB_QUERY_HANDLER_HPP

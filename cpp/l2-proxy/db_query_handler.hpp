#ifndef DB_QUERY_HANDLER_HPP
#define DB_QUERY_HANDLER_HPP

#include "config.hpp"
#include "db_query_executor.hpp"
#include "nlohmann/json.hpp"
#include <map>
#include <memory>
#include <string>

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

  [[nodiscard]] bool is_enabled() const { return !m_executors.empty(); }

  // True when every configured database has an initialized executor. The
  // worker retries init() until this holds so a fast-starting database (e.g.
  // PostgreSQL) cannot mask a slow one (Oracle cold start).
  [[nodiscard]] bool all_configured() const {
    return m_expected_count > 0 && m_executors.size() >= m_expected_count;
  }

  // Executes one DbQueryContract request. Fills status_code with the HTTP
  // status of the DB gateway response and body with its JSON body
  // (success body or ErrorResponse object).
  void handle_request(const json &request, int &status_code, json &body);

  // Wires the pool gauge family into every (future) executor created by
  // init(). Safe to call before init(); executors already created keep their
  // current pointer.
  void set_pool_metrics(prometheus::Family<prometheus::Gauge> *pool_metrics);

private:
  std::map<std::string, std::unique_ptr<DbQueryExecutor>> m_executors;
  size_t m_expected_count = 0;
  prometheus::Family<prometheus::Gauge> *m_pool_metrics = nullptr;
};

#endif // DB_QUERY_HANDLER_HPP

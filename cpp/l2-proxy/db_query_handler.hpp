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

  // Creates an executor for every configured database. Returns false when no
  // database could be initialized (the worker then skips the DB subscription).
  bool init(const std::vector<DbConfig> &databases);

  [[nodiscard]] bool is_enabled() const { return !m_executors.empty(); }

  // Executes one DbQueryContract request. Fills status_code with the HTTP
  // status of the DB gateway response and body with its JSON body
  // (success body or ErrorResponse object).
  void handle_request(const json &request, int &status_code, json &body);

private:
  std::map<std::string, std::unique_ptr<DbQueryExecutor>> m_executors;
};

#endif // DB_QUERY_HANDLER_HPP

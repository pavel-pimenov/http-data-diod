#include "db_query_handler.hpp"
#include "db_query_utils.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include <chrono>
#include <format>

namespace {
uint64_t steady_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
} // namespace

bool DbQueryHandler::init(const std::vector<DbConfig> &databases) {
  m_expected_count = databases.size();
  for (const DbConfig &db : databases) {
    if (m_executors.contains(db.m_name)) {
      continue;
    }
    auto executor = create_db_query_executor(db);
    if (!executor || !executor->init()) {
      Logger::error("DB handler: failed to init executor for database '{}', "
                    "skipping (will retry)",
                    db.m_name);
      continue;
    }
    m_executors.emplace(db.m_name, std::move(executor));
  }
  if (m_executors.empty()) {
    Logger::warn("DB handler: no database executor initialized");
    return false;
  }
  Logger::info("DB handler: ready with {}/{} database(s)", m_executors.size(),
               m_expected_count);
  return true;
}

void DbQueryHandler::handle_request(const json &request, int &status_code,
                                    json &body) {
  auto parsed = parse_db_query_request(request);
  if (!parsed) {
    status_code = 400;
    body = make_db_error_body(status_code, "BAD_REQUEST", parsed.error());
    return;
  }
  const DbQueryRequest &req = *parsed;

  DbQueryExecutor *executor = nullptr;
  if (req.m_db.empty() && m_executors.size() == 1) {
    executor = m_executors.begin()->second.get();
  } else if (const auto it = m_executors.find(req.m_db);
             it != m_executors.end()) {
    executor = it->second.get();
  }
  if (!executor) {
    status_code = 404;
    body = make_db_error_body(status_code, "UNKNOWN_DATABASE",
                              std::format("Unknown database '{}'", req.m_db));
    return;
  }

  if (req.m_type == DbQueryContract::kTypePing) {
    const uint64_t start_ms = steady_ms();
    const bool ok = executor->ping(req.m_timeout_ms);
    const uint64_t latency_ms = steady_ms() - start_ms;
    status_code = ok ? 200 : 503;
    body = ok ? make_db_ping_response(executor->db_name(), latency_ms)
              : make_db_error_body(status_code, "DB_UNAVAILABLE",
                                   "Database ping failed");
    return;
  }

  const int timeout_ms =
      req.m_timeout_ms > 0 ? req.m_timeout_ms : executor->default_timeout_ms();
  const int max_rows =
      req.m_max_rows > 0 ? req.m_max_rows : executor->default_max_rows();
  int exec_status = 200;
  json exec_body =
      executor->execute_query(req.m_sql, req.m_params, timeout_ms, max_rows,
                              exec_status);
  status_code = exec_status;
  body = exec_body;
}

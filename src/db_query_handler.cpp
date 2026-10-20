#include "db_query_handler.hpp"
#include "config.hpp"
#include "db_query_executor.hpp"
#include "db_query_utils.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "time_utils.hpp"
#include <cstdint>
#include <format>
#include <mutex>
#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <utility>
#include <vector>

bool DbQueryHandler::init(const std::vector<DbConfig> &databases) {
  {
    const std::scoped_lock lock(m_state.m_mutex);
    m_state.m_expected_count = databases.size();
    m_state.m_configured_names.clear();
    m_state.m_configured_names.reserve(databases.size());
    for (const auto &db : databases) {
      m_state.m_configured_names.push_back(db.m_name);
    }
  }
  for (const auto &db : databases) {
    {
      const std::scoped_lock lock(m_state.m_mutex);
      if (m_state.m_executors.contains(db.m_name)) {
        continue;
      }
    }
    // Created outside the lock: creating an executor may block (libpq connect
    // probe, ODPI pool creation), so holding m_state.m_mutex across it would stall
    // request dispatch on the pool threads.
    auto executor = create_db_query_executor(db);
    if (!executor || !executor->init()) {
      Logger::error("DB handler: failed to init executor for database '{}', "
                    "skipping (will retry)",
                    db.m_name);
      continue;
    }
    executor->set_error_metrics(m_errors_by_class);
    executor->set_pool_metrics(m_pool_metrics);
    executor->set_pool_saturation_metrics(m_pool_capacity, m_pool_exhausted,
                                          m_pool_acquire_duration);
    const std::scoped_lock lock(m_state.m_mutex);
    if (!m_state.m_executors.contains(db.m_name)) {
      m_state.m_executors.try_emplace(db.m_name, std::move(executor));
    }
  }
  const std::scoped_lock lock(m_state.m_mutex);
  if (m_state.m_executors.empty()) {
    Logger::warn("DB handler: no database executor initialized");
    return false;
  }
  Logger::info("DB handler: ready with {}/{} database(s)", m_state.m_executors.size(),
               m_state.m_expected_count);
  return true;
}

void DbQueryHandler::set_error_metrics(
    prometheus::Family<prometheus::Counter> *errors_by_class) {
  m_errors_by_class = errors_by_class;
}

void DbQueryHandler::set_pool_metrics(
    prometheus::Family<prometheus::Gauge> *pool_metrics) {
  m_pool_metrics = pool_metrics;
}

void DbQueryHandler::set_pool_saturation_metrics(
    prometheus::Family<prometheus::Gauge> *pool_capacity,
    prometheus::Family<prometheus::Counter> *pool_exhausted,
    prometheus::Family<prometheus::Histogram> *pool_acquire_duration) {
  m_pool_capacity = pool_capacity;
  m_pool_exhausted = pool_exhausted;
  m_pool_acquire_duration = pool_acquire_duration;
}

std::vector<std::string> DbQueryHandler::configured_databases() const {
  const std::scoped_lock lock(m_state.m_mutex);
  return m_state.m_configured_names;
}

std::vector<std::string> DbQueryHandler::ready_databases() const {
  const std::scoped_lock lock(m_state.m_mutex);
  std::vector<std::string> names;
  names.reserve(m_state.m_executors.size());
  for (const auto &[name, executor] : m_state.m_executors) {
    (void)executor;
    names.push_back(name);
  }
  return names;
}

void DbQueryHandler::handle_request(const json &request, int &status_code,
                                    json &body) {
  auto parsed = parse_db_query_request(request);
  if (!parsed) {
    status_code = 400;
    body = make_db_error_body("BAD_REQUEST", parsed.error());
    return;
  }
  const auto &req = *parsed;

  DbQueryExecutor *executor = nullptr;
  {
    const std::scoped_lock lock(m_state.m_mutex);
    if (req.m_db.empty() && m_state.m_executors.size() == 1) {
      executor = m_state.m_executors.begin()->second.get();
    } else if (const auto it = m_state.m_executors.find(req.m_db);
               it != m_state.m_executors.end()) {
      executor = it->second.get();
    }
  }
  if (!executor) {
    status_code = 404;
    body = make_db_error_body("UNKNOWN_DATABASE",
                              std::format("Unknown database '{}'", req.m_db));
    return;
  }

  if (req.m_type == DbQueryContract::kTypePing) {
    const uint64_t start_ms = TimeUtils::steady_ms();
    const bool ok = executor->ping(req.m_timeout_ms);
    const uint64_t latency_ms = TimeUtils::steady_ms() - start_ms;
    status_code = ok ? 200 : 503;
    body = ok ? make_db_ping_response(executor->db_name(), latency_ms)
              : make_db_error_body("DB_UNAVAILABLE",
                                   "Database ping failed");
    return;
  }

  const int timeout_ms =
      resolve_positive_or(req.m_timeout_ms, executor->default_timeout_ms());
  const int max_rows =
      resolve_positive_or(req.m_max_rows, executor->default_max_rows());
  int exec_status = 200;
  const json exec_body =
      executor->execute_query(req.m_sql, req.m_params, timeout_ms, max_rows,
                              exec_status);
  status_code = exec_status;
  body = exec_body;
}

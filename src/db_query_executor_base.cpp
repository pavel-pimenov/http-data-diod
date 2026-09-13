#include "db_query_executor_base.hpp"
#include "db_query_utils.hpp"
#include "time_utils.hpp"
#include <utility>

DbExecutorBase::DbExecutorBase(DbConfig db) : m_db(std::move(db)) {}

int DbExecutorBase::default_timeout_ms() const {
  return m_db.m_query_timeout_ms;
}

int DbExecutorBase::default_max_rows() const { return m_db.m_max_rows; }

const std::string &DbExecutorBase::db_name() const { return m_db.m_name; }

void DbExecutorBase::set_pool_metrics(
    prometheus::Family<prometheus::Gauge> *pool_metrics) {
  m_pool_metrics = pool_metrics;
  refresh_pool_gauges();
}

void DbExecutorBase::set_db_pool_gauges(double idle, double active) {
  if (!m_pool_metrics) {
    return;
  }
  m_pool_metrics->Add({{"db", m_db.m_name}, {"state", "idle"}}).Set(idle);
  m_pool_metrics->Add({{"db", m_db.m_name}, {"state", "active"}}).Set(active);
}

json DbExecutorBase::build_query_response(const json &columns_json,
                                          DbRowCollector &rows,
                                          uint64_t start_ms) const {
  const size_t row_count = rows.size();
  const bool truncated = rows.truncated();
  const uint64_t end_ms = TimeUtils::steady_ms();
  return make_db_query_response(m_db.m_name, columns_json, rows.take_rows(),
                                row_count, truncated, end_ms - start_ms);
}
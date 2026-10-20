#include "db_query_executor_base.hpp"
#include "config.hpp"
#include "db_query_utils.hpp"
#include "json_utils.hpp"
#include "metrics_manager.hpp"
#include "time_utils.hpp"
#include <cstddef>
#include <cstdint>
#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <string>
#include <utility>

DbExecutorBase::DbExecutorBase(DbConfig db) : m_db(std::move(db)) {}

int DbExecutorBase::default_timeout_ms() const {
  return m_db.m_query_timeout_ms;
}

int DbExecutorBase::default_max_rows() const { return m_db.m_max_rows; }

const std::string &DbExecutorBase::db_name() const { return m_db.m_name; }

void DbExecutorBase::set_error_metrics(
    prometheus::Family<prometheus::Counter> *errors_by_class) {
  m_errors_by_class = errors_by_class;
}

void DbExecutorBase::count_query_error(
    const std::string &error_class) const {
  if (m_errors_by_class == nullptr) {
    return;
  }
  m_errors_by_class->Add({{"error_class", error_class}}).Increment();
}

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

void DbExecutorBase::set_pool_saturation_metrics(
    prometheus::Family<prometheus::Gauge> *pool_capacity,
    prometheus::Family<prometheus::Counter> *pool_exhausted,
    prometheus::Family<prometheus::Histogram> *pool_acquire_duration) {
  m_pool_capacity = pool_capacity;
  m_pool_exhausted = pool_exhausted;
  m_pool_acquire_duration = pool_acquire_duration;
  // Materialize the per-database series even before the first acquire: a
  // labeled family is only exported after the first Add().
  if (m_pool_capacity) {
    m_pool_capacity->Add({{"db", m_db.m_name}}).Set(
        static_cast<double>(m_db.m_pool_max));
  }
  if (m_pool_exhausted) {
    m_pool_exhausted->Add({{"db", m_db.m_name}});
  }
  if (m_pool_acquire_duration) {
    m_pool_acquire_duration->Add({{"db", m_db.m_name}},
                                 latency_buckets_ms_to_10s());
  }
}

void DbExecutorBase::publish_pool_exhausted() {
  if (m_pool_exhausted) {
    m_pool_exhausted->Add({{"db", m_db.m_name}}).Increment();
  }
}

void DbExecutorBase::observe_pool_acquire(double seconds) {
  if (m_pool_acquire_duration) {
    m_pool_acquire_duration->Add({{"db", m_db.m_name}},
                                 latency_buckets_ms_to_10s())
        .Observe(seconds);
  }
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
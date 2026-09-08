// Unit tests for DbExecutorBase (db_query_executor_base.cpp): the
// driver-agnostic base of the DB Gateway executors. Since the base does not
// depend on ODPI-C / libpq, it is exercised through a minimal stub subclass
// that only implements the pure-virtual surface. Linked into test_components
// alongside db_query_executor_base.cpp.

#include "db_query_executor_base.hpp"
#include "db_query_executor.hpp"
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <prometheus/registry.h>

namespace {

class StubExecutor final : public DbExecutorBase {
public:
  using DbExecutorBase::DbExecutorBase;

  bool init() override { return true; }

  bool is_ready() const override { return true; }

  json execute_query(const std::string & /*sql*/, const json & /*params*/,
                     int /*timeout_ms*/, int /*max_rows*/,
                     int &status_code) override {
    status_code = 200;
    return json::object();
  }

  bool ping(int /*timeout_ms*/) override { return true; }

  int m_refresh_gauges_calls = 0;

protected:
  void refresh_pool_gauges() override { ++m_refresh_gauges_calls; }
};

DbConfig make_db() {
  DbConfig db;
  db.m_name = "pg-test";
  db.m_driver = "postgres";
  db.m_query_timeout_ms = 1234;
  db.m_max_rows = 111;
  return db;
}

} // namespace

TEST_CASE("DbExecutorBase: configured defaults are exposed",
          "[db-executor-base]") {
  StubExecutor ex(make_db());
  REQUIRE(ex.default_timeout_ms() == 1234);
  REQUIRE(ex.default_max_rows() == 111);
  REQUIRE(ex.db_name() == "pg-test");
}

TEST_CASE("DbExecutorBase: set_pool_metrics triggers refresh",
          "[db-executor-base]") {
  StubExecutor ex(make_db());
  auto registry = std::make_shared<prometheus::Registry>();
  auto &family = prometheus::BuildGauge()
                     .Name("l2_worker_db_pool_connections")
                     .Help("test")
                     .Register(*registry);
  REQUIRE(ex.m_refresh_gauges_calls == 0);
  ex.set_pool_metrics(&family);
  REQUIRE(ex.m_refresh_gauges_calls == 1);
}

TEST_CASE("DbExecutorBase: set_db_pool_gauges publishes idle/active",
          "[db-executor-base]") {
  StubExecutor ex(make_db());
  auto registry = std::make_shared<prometheus::Registry>();
  auto &family = prometheus::BuildGauge()
                     .Name("l2_worker_db_pool_connections")
                     .Help("test")
                     .Register(*registry);
  ex.set_pool_metrics(&family);
  ex.set_db_pool_gauges(2.0, 3.0);

  const auto collected = family.Collect();
  REQUIRE(collected.front().metric.size() == 2);
  double idle = 0.0;
  double active = 0.0;
  for (const prometheus::ClientMetric &m : collected.front().metric) {
    REQUIRE(m.label.size() == 2);
    const bool is_idle = m.label[1].value == "idle";
    if (is_idle) {
      idle = m.gauge.value;
    } else {
      active = m.gauge.value;
    }
  }
  REQUIRE(idle == 2.0);
  REQUIRE(active == 3.0);
}

TEST_CASE("DbExecutorBase: set_db_pool_gauges no-op without metrics",
          "[db-executor-base]") {
  StubExecutor ex(make_db());
  REQUIRE_NOTHROW(ex.set_db_pool_gauges(1.0, 1.0));
}

TEST_CASE("DbExecutorBase: setting metrics to nullptr disables gauges",
          "[db-executor-base]") {
  StubExecutor ex(make_db());
  auto registry = std::make_shared<prometheus::Registry>();
  auto &family = prometheus::BuildGauge()
                     .Name("l2_worker_db_pool_connections")
                     .Help("test")
                     .Register(*registry);
  ex.set_pool_metrics(&family);
  ex.set_pool_metrics(nullptr);
  ex.set_db_pool_gauges(5.0, 5.0);
  REQUIRE(family.Collect().empty());
}
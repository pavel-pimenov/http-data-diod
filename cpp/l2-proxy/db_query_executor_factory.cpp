#include "db_query_executor.hpp"
#include "db_query_executor_oracle.hpp"
#include "db_query_executor_postgres.hpp"
#include "logger.hpp"
#include <format>

std::unique_ptr<DbQueryExecutor> create_db_query_executor(const DbConfig &db) {
  if (db.m_driver == "oracle") {
    return std::make_unique<OracleQueryExecutor>(db);
  }
  if (db.m_driver == "postgres") {
    return std::make_unique<PostgresQueryExecutor>(db);
  }
  Logger::warn("DB handler: unknown driver '{}' for database '{}', skipping",
               db.m_driver, db.m_name);
  return nullptr;
}

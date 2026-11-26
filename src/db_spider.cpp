#include "db_spider.hpp"
#include "db_query_handler.hpp"
#include "db_query_utils.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "time_utils.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <string>

// Converts a DbConfig into a DbSpider database entry. DbSpider uses the same
// driver pool config as the worker (DbConfig), so a database configured in
// DB_SPIDER_DATABASES is translated to the executor's expected shape.
static DbConfig spider_db_to_db_config(const Config::Spider::Db &db) {
  DbConfig cfg;
  cfg.m_name = db.m_name;
  cfg.m_driver = db.m_driver;
  cfg.m_host = db.m_host;
  cfg.m_port = db.m_port;
  cfg.m_service = db.m_service;
  cfg.m_database = db.m_database;
  cfg.m_user = db.m_user;
  cfg.m_password = db.m_password;
  cfg.m_pool_min = db.m_pool_min;
  cfg.m_pool_max = db.m_pool_max;
  // Spider queries set their own timeout/rows per request; use generous
  // defaults so the handler does not clamp them.
  cfg.m_query_timeout_ms = 60000;
  cfg.m_max_rows = 1000000;
  return cfg;
}

DbSpider::DbSpider(AppContext &context)
    : m_ctx(context), m_databases(context.m_config.m_spider.m_databases) {
  m_handler = std::make_unique<DbQueryHandler>();
  std::vector<DbConfig> db_configs;
  db_configs.reserve(m_databases.size());
  for (const auto &db : m_databases) {
    db_configs.push_back(spider_db_to_db_config(db));
  }
  m_handler->set_pool_metrics(&m_ctx.m_worker.m_metrics->m_db_pool_connections);
  m_handler->set_error_metrics(&m_ctx.m_worker.m_metrics->m_db_query_errors_total);
  m_handler->set_pool_saturation_metrics(
      &m_ctx.m_worker.m_metrics->m_db_pool_capacity,
      &m_ctx.m_worker.m_metrics->m_db_pool_exhausted_total,
      &m_ctx.m_worker.m_metrics->m_db_pool_acquire_duration_seconds);
  if (!db_configs.empty()) {
    m_handler->init(db_configs);
  }
}

DbSpider::~DbSpider() = default;

json DbSpider::poll_database(Config::Spider::Db &db) {
  json result = json::object();
  result["db"] = db.m_name;
  result["driver"] = db.m_driver;
  result["timestamp"] = TimeUtils::epoch_us();
  json queries = json::object();
  for (const auto &query : db.m_queries) {
    // Выполнить запрос потоково и записать файл. stream_output сам пишет
    // error-объект в файл при ошибке и возвращает HTTP-статус результата.
    const int status = stream_output(db, query);
    json file = json::object();
    file["generated_at"] = TimeUtils::epoch_us();
    file["db"] = db.m_name;
    file["query"] = query.m_name;
    file["status"] = status == http_status::kOk
                         ? DbResponseContract::kStatusOk
                         : DbResponseContract::kStatusError;
    queries[query.m_name] = file;
    m_ctx.m_spider.m_metrics->m_queries_total
        .Add({{"db", db.m_name},
              {"query", query.m_name},
              {"status", status == http_status::kOk ? "ok" : "error"}})
        .Increment();
    if (status != http_status::kOk) {
      Logger::warn("DB spider: query '{}' on '{}' failed (status {})",
                   query.m_name, db.m_name, status);
    }
  }
  result["queries"] = std::move(queries);
  return result;
}

int DbSpider::stream_output(Config::Spider::Db &db,
                            const Config::Spider::DbQuery &query) {
  std::ofstream out;
  std::string path;
  // Sanitize query/db names into file-name-safe segments.
  auto safe = [](const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
      const bool allowed =
          (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
      out.push_back(allowed ? c : '_');
    }
    return out;
  };
  // Writes a small error JSON object (instead of a row stream) into the file.
  auto write_error = [&](int code, const std::string &message) {
    out.close();
    std::ofstream err_out(path);
    if (!err_out.is_open()) {
      return;
    }
    err_out << "{\"generated_at\":" << TimeUtils::epoch_us()
            << ",\"db\":" << json(db.m_name).dump()
            << ",\"query\":" << json(query.m_name).dump()
            << R"(,"status":"error","result":{"error":{"code":)"
            << json(code).dump()
            << ",\"message\":" << json(message).dump() << "}}}";
    err_out.close();
  };

  try {
    std::filesystem::create_directories(m_ctx.m_config.m_spider.m_output_dir);
    path = std::filesystem::path(m_ctx.m_config.m_spider.m_output_dir) /
           (safe(db.m_name) + "_" + safe(query.m_name) + ".json");
    out.open(path);
    if (!out.is_open()) {
      Logger::error("DB spider: failed to open output file '{}'", path);
      return http_status::kInternalServerError;
    }
    out << "{\"generated_at\":" << TimeUtils::epoch_us()
        << ",\"db\":" << json(db.m_name).dump()
        << ",\"query\":" << json(query.m_name).dump()
        << R"(,"status":"ok","result":{"rows":)";
    // Stream the query: rows are written incrementally (each serialised via
    // json.dump with error_handler_t::replace so invalid UTF-8 from Oracle
    // text fields is replaced by U+FFFD instead of aborting), joined into a
    // JSON array stream-wise. The full result set is never materialised.
    int rows_written = 0;
    auto row_cb = [&](json &&row) {
      if (!out.is_open()) {
        return false;
      }
      out << (rows_written == 0 ? "[" : ",")
          << row.dump(-1, ' ', false, json::error_handler_t::replace);
      ++rows_written;
      return true;
    };
    // Build the DbQueryContract request and run it through the shared handler
    // in streaming mode (row-by-row, no full materialisation in memory).
    json request;
    request[DbQueryContract::kType] = DbQueryContract::kTypeQuery;
    request[DbQueryContract::kRequestId] = "spider";
    request[DbQueryContract::kDb] = db.m_name;
    request[DbQueryContract::kSql] = query.m_sql;
    request[DbQueryContract::kTimeoutMs] = query.m_timeout_ms;
    request[DbQueryContract::kMaxRows] = query.m_max_rows;
    int status_code = 0;
    json columns;
    const int row_count =
        m_handler->stream_request(request, status_code, columns,
                                  std::move(row_cb));
    if (status_code != http_status::kOk) {
      Logger::warn("DB spider: query '{}' on '{}' failed (status {})",
                   query.m_name, db.m_name, status_code);
      write_error(status_code,
                  std::format("query '{}' failed with status {}",
                              query.m_name, status_code));
      return status_code;
    }
    // columns were captured by the executor; write them after the rows array.
    out << "],\"columns\":" << columns.dump(-1, ' ', false,
                                             json::error_handler_t::replace)
        << ",\"row_count\":" << row_count << "}}";
    out.close();
    return http_status::kOk;
  } catch (const std::exception &e) {
    Logger::error("DB spider: failed to write output: {}", e.what());
    write_error(http_status::kInternalServerError, e.what());
    return http_status::kInternalServerError;
  }
}

json DbSpider::run_poll() {
  json result = json::object();
  result["generated_at"] = TimeUtils::epoch_us();
  result["interval_seconds"] = m_ctx.m_config.m_spider.m_interval_seconds;
  json databases = json::array();
  for (auto &db : m_databases) {
    databases.push_back(poll_database(db));
  }
  result["databases"] = std::move(databases);
  return result;
}

void DbSpider::record_metrics(const json &result,
                              const std::chrono::steady_clock::time_point &start) {
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  m_ctx.m_spider.m_metrics->m_last_poll_duration_seconds.Set(elapsed);
  // Healthy when the poll produced a "databases" array (no fatal failure).
  const bool ok = result.contains("databases");
  m_ctx.m_spider.m_metrics->m_healthy.Set(ok ? 1.0 : 0.0);
  m_ctx.m_spider.m_metrics->m_polls_total
      .Add({{"outcome", ok ? "ok" : "error"}})
      .Increment();
  if (ok) {
    m_ctx.m_spider.m_metrics->m_last_success_timestamp.Set(
        static_cast<double>(TimeUtils::epoch_s()));
  }
}

void DbSpider::run(const std::stop_token &st) {
  Logger::info("DB spider started: {} database(s), interval {}s, output '{}'",
               m_databases.size(), m_ctx.m_config.m_spider.m_interval_seconds,
               m_ctx.m_config.m_spider.m_output_dir);
  const std::chrono::seconds interval(m_ctx.m_config.m_spider.m_interval_seconds);
  while (!st.stop_requested()) {
    // Poll only when every configured database has a ready executor (the
    // Oracle pool may still be starting up on the first iterations). Until
    // then sleep briefly and retry instead of writing a file full of 503s.
    if (!m_handler->is_enabled() || !m_handler->all_ready()) {
      Logger::debug("DB spider: waiting for database executor(s) to be ready");
      std::this_thread::sleep_for(std::chrono::seconds(5));
      continue;
    }
    const auto start = std::chrono::steady_clock::now();
    const json result = run_poll();
    record_metrics(result, start);
    if (interval.count() <= 0) {
      break;
    }
    std::this_thread::sleep_for(interval);
  }
  Logger::info("DB spider stopped");
}

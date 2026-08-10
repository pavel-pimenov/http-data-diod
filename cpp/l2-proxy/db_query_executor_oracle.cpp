#include "db_query_executor_oracle.hpp"
#include "db_query_utils.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include <base64.hpp>
#include <dpi.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace {
constexpr uint32_t g_max_lob_bytes = 1024 * 1024;
constexpr uint32_t g_max_inline_bytes = 1024 * 1024;

uint64_t executor_steady_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string oracle_type_name(dpiOracleTypeNum type) {
  switch (type) {
  case DPI_ORACLE_TYPE_VARCHAR:
    return "VARCHAR2";
  case DPI_ORACLE_TYPE_NVARCHAR:
    return "NVARCHAR2";
  case DPI_ORACLE_TYPE_CHAR:
    return "CHAR";
  case DPI_ORACLE_TYPE_NCHAR:
    return "NCHAR";
  case DPI_ORACLE_TYPE_ROWID:
    return "ROWID";
  case DPI_ORACLE_TYPE_UROWID:
    return "UROWID";
  case DPI_ORACLE_TYPE_RAW:
    return "RAW";
  case DPI_ORACLE_TYPE_NUMBER:
    return "NUMBER";
  case DPI_ORACLE_TYPE_NATIVE_INT:
    return "INTEGER";
  case DPI_ORACLE_TYPE_NATIVE_UINT:
    return "UINTEGER";
  case DPI_ORACLE_TYPE_NATIVE_FLOAT:
    return "FLOAT";
  case DPI_ORACLE_TYPE_NATIVE_DOUBLE:
    return "DOUBLE";
  case DPI_ORACLE_TYPE_BOOLEAN:
    return "BOOLEAN";
  case DPI_ORACLE_TYPE_DATE:
    return "DATE";
  case DPI_ORACLE_TYPE_TIMESTAMP:
    return "TIMESTAMP";
  case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
    return "TIMESTAMP WITH TIME ZONE";
  case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
    return "TIMESTAMP WITH LOCAL TIME ZONE";
  case DPI_ORACLE_TYPE_INTERVAL_DS:
    return "INTERVAL DAY TO SECOND";
  case DPI_ORACLE_TYPE_INTERVAL_YM:
    return "INTERVAL YEAR TO MONTH";
  case DPI_ORACLE_TYPE_CLOB:
    return "CLOB";
  case DPI_ORACLE_TYPE_NCLOB:
    return "NCLOB";
  case DPI_ORACLE_TYPE_BLOB:
    return "BLOB";
  case DPI_ORACLE_TYPE_BFILE:
    return "BFILE";
  case DPI_ORACLE_TYPE_LONG_VARCHAR:
    return "LONG";
  case DPI_ORACLE_TYPE_LONG_NVARCHAR:
    return "LONG";
  case DPI_ORACLE_TYPE_LONG_RAW:
    return "LONG RAW";
  case DPI_ORACLE_TYPE_XMLTYPE:
    return "XMLTYPE";
  default:
    return std::format("UNKNOWN({})", static_cast<int>(type));
  }
}

std::string format_timestamp(const dpiTimestamp &t) {
  std::string result =
      std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", t.year, t.month,
                  t.day, t.hour, t.minute, t.second);
  if (t.fsecond != 0) {
    result += std::format(".{:09}", t.fsecond);
  }
  if (t.tzHourOffset != 0 || t.tzMinuteOffset != 0) {
    const char sign = t.tzHourOffset < 0 ? '-' : '+';
    result += std::format(" {}{:02}:{:02}", sign, std::abs(t.tzHourOffset),
                          std::abs(t.tzMinuteOffset));
  }
  return result;
}

std::string format_interval_ds(const dpiIntervalDS &i) {
  std::string result = std::format("{} {:02}:{:02}:{:02}", i.days, i.hours,
                                   i.minutes, i.seconds);
  if (i.fseconds != 0) {
    result += std::format(".{:09}", i.fseconds);
  }
  return result;
}

std::string format_interval_ym(const dpiIntervalYM &i) {
  return std::format("{}-{:02}", i.years, i.months);
}
} // namespace

struct OracleQueryExecutor::Impl {
  const DbConfig m_db;
  dpiContext *m_context = nullptr;
  dpiErrorInfo m_error_info{};
  dpiPool *m_pool = nullptr;

  explicit Impl(DbConfig db) : m_db(std::move(db)) {}

  ~Impl() {
    if (m_pool) {
      dpiPool_release(m_pool);
    }
    if (m_context) {
      dpiContext_destroy(m_context);
    }
  }

  std::string describe_error() {
    if (m_context) {
      dpiContext_getError(m_context, &m_error_info);
    }
    const std::string message(m_error_info.message,
                              m_error_info.messageLength);
    return std::format("{} [{}] {}", message, m_error_info.code,
                       m_error_info.fnName ? m_error_info.fnName : "");
  }

  bool init() {
    if (dpiContext_create(DPI_MAJOR_VERSION, DPI_MINOR_VERSION, &m_context,
                          &m_error_info) < 0) {
      Logger::error("DB executor '{}': failed to create ODPI-C context: {}",
                    m_db.m_name, describe_error());
      return false;
    }
    dpiPoolCreateParams pool_params{};
    if (dpiContext_initPoolCreateParams(m_context, &pool_params) < 0) {
      Logger::error("DB executor '{}': failed to init pool params: {}",
                    m_db.m_name, describe_error());
      return false;
    }
    pool_params.minSessions = static_cast<uint32_t>(m_db.m_pool_min);
    pool_params.maxSessions = static_cast<uint32_t>(m_db.m_pool_max);
    pool_params.sessionIncrement = 1;
    pool_params.homogeneous = 1;
    const std::string connect_string =
        std::format("{}:{}/{}", m_db.m_host, m_db.m_port, m_db.m_service);
    if (dpiPool_create(m_context, m_db.m_user.data(),
                       static_cast<uint32_t>(m_db.m_user.size()),
                       m_db.m_password.data(),
                       static_cast<uint32_t>(m_db.m_password.size()),
                       connect_string.data(),
                       static_cast<uint32_t>(connect_string.size()), nullptr,
                       &pool_params, &m_pool) < 0) {
      Logger::error("DB executor '{}': failed to create pool for '{}': {}",
                    m_db.m_name, connect_string, describe_error());
      return false;
    }
    Logger::info("DB executor '{}': pool ready ({}..{} sessions, connect {} )",
                 m_db.m_name, m_db.m_pool_min, m_db.m_pool_max, connect_string);
    return true;
  }

  std::optional<dpiConn *> acquire_conn(int timeout_ms) {
    dpiConnCreateParams create_params{};
    if (dpiContext_initConnCreateParams(m_context, &create_params) < 0) {
      Logger::warn("DB executor '{}': failed to init conn params: {}",
                   m_db.m_name, describe_error());
      return std::nullopt;
    }
    dpiConn *conn = nullptr;
    if (dpiPool_acquireConnection(m_pool, nullptr, 0, nullptr, 0,
                                  &create_params, &conn) < 0) {
      Logger::warn("DB executor '{}': failed to acquire connection: {}",
                   m_db.m_name, describe_error());
      return std::nullopt;
    }
    if (timeout_ms > 0) {
      dpiConn_setCallTimeout(conn, static_cast<uint32_t>(timeout_ms));
    }
    return conn;
  }
};

OracleQueryExecutor::OracleQueryExecutor(DbConfig db)
    : m_impl(std::make_unique<Impl>(std::move(db))) {}

OracleQueryExecutor::~OracleQueryExecutor() = default;

bool OracleQueryExecutor::init() { return m_impl->init(); }

int OracleQueryExecutor::default_timeout_ms() const {
  return m_impl->m_db.m_query_timeout_ms;
}

int OracleQueryExecutor::default_max_rows() const { return m_impl->m_db.m_max_rows; }

const std::string &OracleQueryExecutor::db_name() const { return m_impl->m_db.m_name; }

namespace {
struct ColumnPlan {
  std::string m_name;
  dpiOracleTypeNum m_oracle_type = DPI_ORACLE_TYPE_NONE;
  dpiNativeTypeNum m_native_type = DPI_NATIVE_TYPE_NULL;
  bool m_is_blob = false;
  uint32_t m_buffer_size = 0;
};

struct StmtGuard {
  dpiStmt *m_stmt = nullptr;
  ~StmtGuard() {
    if (m_stmt) {
      dpiStmt_release(m_stmt);
    }
  }
};
} // namespace

json OracleQueryExecutor::execute_query(const std::string &sql, const json &params,
                                    int timeout_ms, int max_rows,
                                    int &status_code) {
  status_code = 200;
  const uint64_t start_ms = executor_steady_ms();

  auto maybe_conn = m_impl->acquire_conn(timeout_ms);
  if (!maybe_conn) {
    status_code = 503;
    return make_db_error_body(status_code, "DB_UNAVAILABLE",
                              "Failed to acquire a database connection: " +
                                  m_impl->describe_error());
  }
  dpiConn *conn = *maybe_conn;

  dpiStmt *stmt_raw = nullptr;
  if (dpiConn_prepareStmt(conn, 0, sql.data(),
                          static_cast<uint32_t>(sql.size()), nullptr, 0,
                          &stmt_raw) < 0) {
    status_code = 422;
    return make_db_error_body(status_code, "SQL_ERROR",
                              m_impl->describe_error());
  }
  StmtGuard stmt_guard{stmt_raw};

  for (const auto &[name, value] : params.items()) {
    dpiData data{};
    std::string str_value;
    dpiNativeTypeNum native = DPI_NATIVE_TYPE_INT64;
    if (value.is_null()) {
      data.isNull = 1;
      native = DPI_NATIVE_TYPE_NULL;
    } else if (value.is_boolean()) {
      data.value.asInt64 = value.get<bool>() ? 1 : 0;
    } else if (value.is_number_integer()) {
      data.value.asInt64 = value.get<int64_t>();
    } else if (value.is_number_float()) {
      data.value.asDouble = value.get<double>();
      native = DPI_NATIVE_TYPE_DOUBLE;
    } else if (value.is_string()) {
      str_value = value.get<std::string>();
      data.value.asBytes.ptr = str_value.data();
      data.value.asBytes.length = static_cast<uint32_t>(str_value.size());
      native = DPI_NATIVE_TYPE_BYTES;
    }
    std::string bind_name = name;
    if (!bind_name.empty() && bind_name.front() == ':') {
      bind_name.erase(0, 1);
    }
    if (dpiStmt_bindValueByName(stmt_raw, bind_name.data(),
                                static_cast<uint32_t>(bind_name.size()),
                                native, &data) < 0) {
      status_code = 422;
      return make_db_error_body(status_code, "SQL_ERROR",
                                std::format("Failed to bind parameter '{}': {}",
                                            name, m_impl->describe_error()));
    }
  }

  uint32_t num_query_columns = 0;
  if (dpiStmt_execute(stmt_raw, DPI_MODE_EXEC_DEFAULT, &num_query_columns) <
      0) {
    status_code = 422;
    return make_db_error_body(status_code, "SQL_ERROR",
                              m_impl->describe_error());
  }
  if (num_query_columns == 0) {
    status_code = 422;
    return make_db_error_body(status_code, "SQL_ERROR",
                              "Statement returned no result set");
  }

  std::vector<ColumnPlan> columns;
  columns.reserve(num_query_columns);
  for (uint32_t pos = 1; pos <= num_query_columns; ++pos) {
    dpiQueryInfo info{};
    if (dpiStmt_getQueryInfo(stmt_raw, pos, &info) < 0) {
      status_code = 422;
      return make_db_error_body(status_code, "SQL_ERROR",
                                m_impl->describe_error());
    }
    ColumnPlan column;
    column.m_name.assign(info.name, info.nameLength);
    column.m_oracle_type = info.typeInfo.oracleTypeNum;
    column.m_is_blob = info.typeInfo.oracleTypeNum == DPI_ORACLE_TYPE_BLOB ||
                       info.typeInfo.oracleTypeNum == DPI_ORACLE_TYPE_BFILE;
    switch (info.typeInfo.oracleTypeNum) {
    case DPI_ORACLE_TYPE_NATIVE_FLOAT:
    case DPI_ORACLE_TYPE_NATIVE_DOUBLE:
      column.m_native_type = DPI_NATIVE_TYPE_DOUBLE;
      break;
    case DPI_ORACLE_TYPE_NATIVE_INT:
    case DPI_ORACLE_TYPE_BOOLEAN:
      column.m_native_type = DPI_NATIVE_TYPE_INT64;
      break;
    case DPI_ORACLE_TYPE_NATIVE_UINT:
      column.m_native_type = DPI_NATIVE_TYPE_UINT64;
      break;
    case DPI_ORACLE_TYPE_DATE:
    case DPI_ORACLE_TYPE_TIMESTAMP:
    case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
    case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
      column.m_native_type = DPI_NATIVE_TYPE_TIMESTAMP;
      break;
    case DPI_ORACLE_TYPE_INTERVAL_DS:
      column.m_native_type = DPI_NATIVE_TYPE_INTERVAL_DS;
      break;
    case DPI_ORACLE_TYPE_INTERVAL_YM:
      column.m_native_type = DPI_NATIVE_TYPE_INTERVAL_YM;
      break;
    case DPI_ORACLE_TYPE_CLOB:
    case DPI_ORACLE_TYPE_NCLOB:
    case DPI_ORACLE_TYPE_BLOB:
    case DPI_ORACLE_TYPE_BFILE:
      column.m_native_type = DPI_NATIVE_TYPE_LOB;
      break;
    default:
      column.m_native_type = DPI_NATIVE_TYPE_BYTES;
      break;
    }
    if (column.m_native_type == DPI_NATIVE_TYPE_BYTES) {
      column.m_buffer_size = info.typeInfo.clientSizeInBytes;
      if (column.m_buffer_size == 0) {
        column.m_buffer_size = 1024;
      }
      column.m_buffer_size = std::min(column.m_buffer_size, g_max_inline_bytes);
    }
    if (dpiStmt_defineValue(stmt_raw, pos, column.m_oracle_type,
                            column.m_native_type, column.m_buffer_size, 1,
                            nullptr) < 0) {
      status_code = 422;
      return make_db_error_body(status_code, "SQL_ERROR",
                                std::format("Failed to define column '{}': {}",
                                            column.m_name,
                                            m_impl->describe_error()));
    }
    columns.push_back(std::move(column));
  }

  json rows = json::array();
  int found = 0;
  uint32_t buffer_row_index = 0;
  bool truncated = false;
  while (true) {
    if (dpiStmt_fetch(stmt_raw, &found, &buffer_row_index) < 0) {
      status_code = 422;
      return make_db_error_body(status_code, "SQL_ERROR",
                                m_impl->describe_error());
    }
    if (!found) {
      break;
    }
    if (rows.size() >= static_cast<size_t>(max_rows)) {
      truncated = true;
      break;
    }
    json row = json::array();
    for (uint32_t pos = 1; pos <= num_query_columns; ++pos) {
      dpiNativeTypeNum native_type = DPI_NATIVE_TYPE_NULL;
      dpiData *data = nullptr;
      if (dpiStmt_getQueryValue(stmt_raw, pos, &native_type, &data) < 0 ||
          !data) {
        status_code = 422;
        return make_db_error_body(status_code, "SQL_ERROR",
                                  m_impl->describe_error());
      }
      const ColumnPlan &column = columns[pos - 1];
      json value = nullptr;
      if (!data->isNull) {
        switch (column.m_native_type) {
        case DPI_NATIVE_TYPE_BYTES:
          value = std::string(data->value.asBytes.ptr, data->value.asBytes.length);
          break;
        case DPI_NATIVE_TYPE_INT64:
          value = data->value.asInt64;
          break;
        case DPI_NATIVE_TYPE_UINT64:
          value = data->value.asUint64;
          break;
        case DPI_NATIVE_TYPE_DOUBLE:
          value = data->value.asDouble;
          break;
        case DPI_NATIVE_TYPE_TIMESTAMP:
          value = format_timestamp(data->value.asTimestamp);
          break;
        case DPI_NATIVE_TYPE_INTERVAL_DS:
          value = format_interval_ds(data->value.asIntervalDS);
          break;
        case DPI_NATIVE_TYPE_INTERVAL_YM:
          value = format_interval_ym(data->value.asIntervalYM);
          break;
        case DPI_NATIVE_TYPE_LOB: {
          dpiLob *lob = data->value.asLOB;
          std::string buffer;
          if (lob) {
            uint64_t lob_size = 0;
            if (dpiLob_getSize(lob, &lob_size) == 0 && lob_size > 0) {
              const uint64_t amount = std::min<uint64_t>(lob_size, g_max_lob_bytes);
              buffer.resize(static_cast<size_t>(amount));
              uint64_t read_len = 0;
              if (dpiLob_readBytes(lob, 1, amount, buffer.data(), &read_len) <
                  0) {
                Logger::warn("DB executor '{}': LOB read failed: {}",
                             m_impl->m_db.m_name, m_impl->describe_error());
                buffer.clear();
              } else {
                buffer.resize(read_len);
              }
            }
          }
          value = column.m_is_blob ? base64::to_base64(buffer) : buffer;
          break;
        }
        default:
          value = nullptr;
          break;
        }
      }
      row.push_back(std::move(value));
    }
    rows.push_back(std::move(row));
  }

  json columns_json = json::array();
  for (const auto &column : columns) {
    columns_json.push_back(json{
        {DbResponseContract::kName, column.m_name},
        {DbResponseContract::kType, oracle_type_name(column.m_oracle_type)}});
  }

  const uint64_t end_ms = executor_steady_ms();
  return make_db_query_response(m_impl->m_db.m_name, columns_json, rows,
                                rows.size(), truncated, end_ms - start_ms);
}

bool OracleQueryExecutor::ping(int timeout_ms) {
  auto maybe_conn = m_impl->acquire_conn(timeout_ms);
  if (!maybe_conn) {
    return false;
  }
  dpiConn *conn = *maybe_conn;
  dpiStmt *stmt_raw = nullptr;
  constexpr const char *kProbeSql = "SELECT 1 FROM DUAL";
  uint32_t num_query_columns = 0;
  const bool ok =
      dpiConn_prepareStmt(conn, 0, kProbeSql, strlen(kProbeSql), nullptr, 0,
                          &stmt_raw) == 0 &&
      dpiStmt_execute(stmt_raw, DPI_MODE_EXEC_DEFAULT, &num_query_columns) ==
          0;
  if (stmt_raw) {
    dpiStmt_release(stmt_raw);
  }
  if (!ok) {
    Logger::warn("DB executor '{}': ping failed: {}", m_impl->m_db.m_name,
                 m_impl->describe_error());
  }
  return ok;
}

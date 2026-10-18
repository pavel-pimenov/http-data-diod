// Live-PostgreSQL coverage for the DB gateway (round 72): unit tests that exec
// a real postgres server on an ephemeral loopback port and drive
// PostgresQueryExecutor and DbQueryHandler against it, so value conversion,
// pool accounting, timeouts, ping and the read-only gateway routing run for
// real. Also the offline fail-fast path (unreachable server).
//
// The server is installed in the builder image via the postgresql apt package
// (src/Dockerfile); tests SKIP when it is not present so local runs without a
// server still pass. The coverage image runs ./test_components, so all
// synchronisation is bounded — nothing may hang the docker build.

#include "config.hpp"
#include "db_query_executor.hpp"
#include "db_query_executor_postgres.hpp"
#include "db_query_handler.hpp"
#include "json_utils.hpp"
#include "logger.hpp"
#include "test_metrics_support.hpp"
#include "test_pg_diag.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <libpq-fe.h>
#include <memory>
#include <prometheus/registry.h>
#include <signal.h>
#include <stdlib.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using pg_test_diag::drop_privileges;
using pg_test_diag::postgres_sql_probe;
using pg_test_diag::redirect_to;
using pg_test_diag::resolve_server_user;
using pg_test_diag::ServerUser;
using test_support::reserve_loopback_port;
using test_support::wait_for_condition;

constexpr int kServerWaitTimeoutMs = 10000;

[[nodiscard]] std::string postgres_bin_dir() {
  const auto has = [](const std::string &dir) {
    return ::access((dir + "/initdb").c_str(), X_OK) == 0 &&
           ::access((dir + "/postgres").c_str(), X_OK) == 0;
  };
  if (has("/usr/local/bin")) {
    return "/usr/local/bin";
  }
  std::error_code ec;
  const std::filesystem::path libdir("/usr/lib/postgresql");
  if (!std::filesystem::exists(libdir, ec)) {
    return "";
  }
  std::vector<std::string> versions;
  for (const auto &entry : std::filesystem::directory_iterator(libdir, ec)) {
    if (entry.is_directory()) {
      versions.push_back(entry.path().filename().string());
    }
  }
  std::ranges::sort(versions,
            [](const std::string &a, const std::string &b) {
              return std::stoi(a) > std::stoi(b);
            });
  for (const auto &version : versions) {
    const std::string dir = "/usr/lib/postgresql/" + version + "/bin";
    if (has(dir)) {
      return dir;
    }
  }
  return "";
}

// One ephemeral postgres cluster shared by the live tests of this TU. Started
// lazily (port chosen per cluster), kept running while the binary is alive.
class PgServer {
public:
  PgServer() = default;
  ~PgServer() { stop(); }
  PgServer(const PgServer &) = delete;
  PgServer &operator=(const PgServer &) = delete;

  [[nodiscard]] int port() const { return m_port; }

  bool ensure_started() {
    if (m_pid > 0) {
      if (alive()) {
        return true;
      }
      // The server exited behind our back (crash, OOM, external kill): the
      // port is stale, so drop the datadir and bring a fresh cluster up.
      Logger::warn("pg live test: shared server died, restarting");
      m_pid = -1;
      cleanup_datadir();
    }
    if (m_failed) {
      return false;
    }
    const bool ok = start();
    if (!ok) {
      m_failed = true;
    }
    return ok;
  }

  void stop() {
    if (m_pid <= 0) {
      return;
    }
    ::kill(m_pid, SIGINT);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int status = 0;
    while (::waitpid(m_pid, &status, WNOHANG) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (::waitpid(m_pid, &status, WNOHANG) == 0) {
      ::kill(m_pid, SIGKILL);
      ::waitpid(m_pid, &status, 0);
    }
    m_pid = -1;
    cleanup_datadir();
  }

private:
  [[nodiscard]] bool alive() const {
    if (m_pid <= 0) {
      return false;
    }
    int status = 0;
    return ::waitpid(m_pid, &status, WNOHANG) == 0;
  }

  [[nodiscard]] static bool run_wait(const std::string &exe,
                                     const std::vector<std::string> &args,
                                     const std::string &logfile,
                                     const ServerUser &target) {
    const pid_t pid = ::fork();
    if (pid == 0) {
      if (!drop_privileges(target) || !redirect_to(logfile.c_str())) {
        _exit(127);
      }
      std::vector<const char *> argv;
      argv.reserve(args.size() + 2);
      argv.push_back(exe.c_str());
      for (const auto &arg : args) {
        argv.push_back(arg.c_str());
      }
      argv.push_back(nullptr);
      ::execv(exe.c_str(), const_cast<char *const *>(argv.data()));
      _exit(127);
    }
    if (pid < 0) {
      pg_test_diag::fail(
          std::format("fork {} failed: {}", exe, std::strerror(errno)));
      return false;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      pg_test_diag::fail(std::format("'{}' failed (rc={}): {}", exe,
                                     WEXITSTATUS(status),
                                     pg_test_diag::log_tail(logfile)));
      return false;
    }
    return true;
  }

  [[nodiscard]] static pid_t spawn_server(const std::string &exe,
                                          const std::vector<std::string> &args,
                                          const std::string &logfile,
                                          const ServerUser &target) {
    const pid_t pid = ::fork();
    if (pid == 0) {
      if (!drop_privileges(target) || !redirect_to(logfile.c_str())) {
        _exit(127);
      }
      std::vector<const char *> argv;
      argv.reserve(args.size() + 2);
      argv.push_back(exe.c_str());
      for (const auto &arg : args) {
        argv.push_back(arg.c_str());
      }
      argv.push_back(nullptr);
      ::execv(exe.c_str(), const_cast<char *const *>(argv.data()));
      _exit(127);
    }
    return pid;
  }

  [[nodiscard]] bool start() {
    if (m_bin_dir.empty()) {
      m_bin_dir = postgres_bin_dir();
      if (m_bin_dir.empty()) {
        Logger::warn("pg live test: postgres binaries not installed, skipping");
        return false;
      }
    }
    const ServerUser target = resolve_server_user();
    if (!target.m_ok) {
      return false;
    }

    // A full /tmp is the classic silent killer here: initdb fails, the tests
    // SKIP and the coverage gate fails without naming the cause.
    if (!pg_test_diag::enough_free_space()) {
      return false;
    }

    m_port = reserve_loopback_port();
    if (m_port <= 0) {
      pg_test_diag::fail("no free loopback port");
      return false;
    }

    const std::string tmpl = "/tmp/pgdb-XXXXXX";
    std::vector<char> dir_tmpl(tmpl.begin(), tmpl.end());
    dir_tmpl.push_back('\0');
    char *dir = ::mkdtemp(dir_tmpl.data());
    if (dir == nullptr) {
      pg_test_diag::fail(std::format("mkdtemp failed: {}", std::strerror(errno)));
      return false;
    }
    m_datadir = dir;

    if (target.m_need_switch &&
        ::chown(m_datadir.c_str(), target.m_uid, target.m_gid) != 0) {
      pg_test_diag::fail(std::format("chown datadir failed: {}",
                                     std::strerror(errno)));
      cleanup_datadir();
      return false;
    }

    const std::string init_log = std::format("/tmp/pg-init-{}.log", m_port);
    if (!run_wait(m_bin_dir + "/initdb",
                  {"-D", m_datadir, "-U", "postgres", "-A", "trust",
                   "--no-locale", "-E", "UTF8"},
                  init_log, target)) {
      pg_test_diag::fail(
          std::format("initdb failed: {}", pg_test_diag::log_tail(init_log)));
      cleanup_datadir();
      return false;
    }

    const std::string server_log = std::format("/tmp/pg-server-{}.log", m_port);
    const pid_t server_pid = spawn_server(
        m_bin_dir + "/postgres",
        {"-D", m_datadir, "-p", std::to_string(m_port), "-h", "127.0.0.1",
         "-k", "/tmp"},
        server_log, target);
    if (server_pid < 0) {
      pg_test_diag::fail(
          std::format("fork postgres failed: {}", std::strerror(errno)));
      cleanup_datadir();
      return false;
    }
    m_pid = server_pid;

    const bool ready = wait_for_condition(
        [this] { return postgres_sql_probe(m_port) && alive(); }, kServerWaitTimeoutMs);
    if (!ready) {
      pg_test_diag::fail(std::format(
          "server at {} never became ready: {}",
          m_port, pg_test_diag::log_tail(server_log)));
      stop();
      cleanup_datadir();
      return false;
    }
    return true;
  }

  void cleanup_datadir() {
    m_pid = -1;
    if (m_datadir.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove_all(m_datadir, ec);
    m_datadir.clear();
  }

  std::string m_bin_dir;
  std::string m_datadir;
  int m_port = 0;
  pid_t m_pid = -1;
  bool m_failed = false;
};

PgServer &shared_pg() {
  static PgServer g_server;
  return g_server;
}

#define REQUIRE_PG()                                                           \
  do {                                                                         \
    if (!shared_pg().ensure_started()) {                                       \
      SKIP("postgres server not available in this environment");               \
    }                                                                          \
  } while (0)

DbConfig make_pg_cfg(const PgServer &server, int pool_max = 5,
                     int query_timeout_ms = 5000) {
  DbConfig db;
  db.m_name = "pg";
  db.m_driver = "postgres";
  db.m_host = "127.0.0.1";
  db.m_port = server.port();
  db.m_database = "postgres";
  db.m_user = "postgres";
  db.m_pool_min = 1;
  db.m_pool_max = pool_max;
  db.m_query_timeout_ms = query_timeout_ms;
  db.m_max_rows = 1000;
  return db;
}

// Raw libpq helper used only for DDL/setup, so data shape is controlled
// independently of the executor under test.
class PqConn {
public:
  explicit PqConn(const PgServer &server) {
    const std::string conninfo = std::format(
        "host=127.0.0.1 port={} dbname=postgres user=postgres "
        "connect_timeout=10",
        server.port());
    m_conn = PQconnectdb(conninfo.c_str());
    REQUIRE(m_conn != nullptr);
    REQUIRE(PQstatus(m_conn) == CONNECTION_OK);
  }

  ~PqConn() {
    if (m_conn != nullptr) {
      PQfinish(m_conn);
    }
  }
  PqConn(const PqConn &) = delete;
  PqConn &operator=(const PqConn &) = delete;

  void exec(const std::string &sql) {
    REQUIRE(m_conn != nullptr);
    PGresult *res = PQexec(m_conn, sql.c_str());
    REQUIRE(res != nullptr);
    const ExecStatusType status = PQresultStatus(res);
    const bool ok = status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
    if (!ok) {
      FAIL(PQresultErrorMessage(res));
    }
    PQclear(res);
  }

private:
  PGconn *m_conn = nullptr;
};

void create_types_table(PqConn &conn) {
  conn.exec("CREATE TABLE IF NOT EXISTS pg_types ("
            "c_bool BOOLEAN, c_i2 SMALLINT, c_i4 INTEGER, c_i8 BIGINT, "
            "c_oid OID, c_f4 REAL, c_f8 DOUBLE PRECISION, c_num NUMERIC(20,4), "
            "c_txt TEXT, c_vc VARCHAR(20), c_bp BPCHAR(5), c_ch \"char\", "
            "c_name NAME, c_bytea BYTEA, c_json JSON, c_jsonb JSONB, "
            "c_uuid UUID, c_date DATE, c_time TIME, c_ts TIMESTAMP, "
            "c_tstz TIMESTAMPTZ, c_timetz TIMETZ)");
  conn.exec("TRUNCATE pg_types");
  conn.exec("INSERT INTO pg_types VALUES ("
            "true, 1, 2, 3, 4, 1.5, 2.5, 12345.6789, "
            "'hello', 'vc', 'bp'::bpchar, 'c'::\"char\", 'myname', "
            "'\\x01ab'::bytea, '{\"a\":1}'::json, '{\"b\":2}'::jsonb, "
            "'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::uuid, "
            "'2024-05-06', '10:11:12', '2024-05-06 10:11:12', "
            "'2024-05-06 10:11:12+03', '10:11:12+03')");
  conn.exec("INSERT INTO pg_types (c_i4, c_txt) VALUES (NULL, NULL)");
}

} // namespace

TEST_CASE("PostgresQueryExecutor: init fails on an unreachable server",
          "[db][db-postgres]") {
  DbConfig db;
  db.m_name = "pg";
  db.m_driver = "postgres";
  db.m_host = "127.0.0.1";
  db.m_port = 1;
  db.m_database = "test";
  db.m_user = "test";
  PostgresQueryExecutor ex(db);
  REQUIRE_FALSE(ex.init());
}

TEST_CASE("PostgresQueryExecutor: live init, ping and plain select",
          "[db-live]") {
  REQUIRE_PG();
  PostgresQueryExecutor ex(make_pg_cfg(shared_pg()));
  REQUIRE(ex.init());
  REQUIRE(ex.is_ready());
  REQUIRE(ex.ping(1000));

  int status = 0;
  const json resp =
      ex.execute_query("SELECT 42 AS answer", json::object(), -1, 10, status);
  REQUIRE(status == 200);
  REQUIRE(resp[DbResponseContract::kStatus] == DbResponseContract::kStatusOk);
  REQUIRE(resp[DbResponseContract::kDb] == "pg");
  REQUIRE(resp[DbResponseContract::kColumns] ==
          json::array({json{{DbResponseContract::kName, "answer"},
                            {DbResponseContract::kType, "INTEGER"}}}));
  REQUIRE(resp[DbResponseContract::kRows] == json::array({json::array({42})}));
  REQUIRE(resp[DbResponseContract::kRowCount] == 1);
  REQUIRE(resp[DbResponseContract::kTruncated] == false);
}

TEST_CASE("PostgresQueryExecutor: value and column-type conversions",
          "[db-live]") {
  REQUIRE_PG();
  {
    PqConn conn(shared_pg());
    create_types_table(conn);
  }
  PostgresQueryExecutor ex(make_pg_cfg(shared_pg()));
  REQUIRE(ex.init());

  int status = 0;
  const json resp = ex.execute_query("SELECT * FROM pg_types ORDER BY c_i4",
                                     json::object(), -1, 10, status);
  REQUIRE(status == 200);
  REQUIRE(resp[DbResponseContract::kRows].size() == 2);
  REQUIRE(resp[DbResponseContract::kColumns].size() == 22);

  const auto &cols = resp[DbResponseContract::kColumns];
  const auto col = [&cols](size_t i) {
    return json{{DbResponseContract::kName, cols[i][DbResponseContract::kName]},
                {DbResponseContract::kType, cols[i][DbResponseContract::kType]}};
  };
  REQUIRE(cols.size() == 22);
  REQUIRE(col(0) == json{{DbResponseContract::kName, "c_bool"},
                         {DbResponseContract::kType, "BOOLEAN"}});
  REQUIRE(col(4) == json{{DbResponseContract::kName, "c_oid"},
                         {DbResponseContract::kType, "OID"}});
  REQUIRE(col(13) == json{{DbResponseContract::kName, "c_bytea"},
                          {DbResponseContract::kType, "BYTEA"}});
  REQUIRE(col(14) == json{{DbResponseContract::kName, "c_json"},
                          {DbResponseContract::kType, "JSON"}});
  REQUIRE(col(15) == json{{DbResponseContract::kName, "c_jsonb"},
                          {DbResponseContract::kType, "JSONB"}});
  REQUIRE(col(16) == json{{DbResponseContract::kName, "c_uuid"},
                          {DbResponseContract::kType, "UUID"}});
  REQUIRE(col(20) == json{{DbResponseContract::kName, "c_tstz"},
                          {DbResponseContract::kType, "TIMESTAMPTZ"}});
  REQUIRE(col(21) == json{{DbResponseContract::kName, "c_timetz"},
                          {DbResponseContract::kType, "TIMETZ"}});

  const auto &full = resp[DbResponseContract::kRows][0];
  REQUIRE(full[0] == true);
  REQUIRE(full[1] == 1);
  REQUIRE(full[2] == 2);
  REQUIRE(full[3] == 3);
  REQUIRE(full[4] == 4);
  REQUIRE(full[5] == 1.5);
  REQUIRE(full[6] == 2.5);
  REQUIRE(full[7] == 12345.6789);
  REQUIRE(full[8] == "hello");
  REQUIRE(full[9] == "vc");
  REQUIRE(full[13] == "Aas=");
  REQUIRE(full[16] == "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11");
  REQUIRE(full[17] == "2024-05-06");
  REQUIRE(full[18] == "10:11:12");
  REQUIRE(full[19] == "2024-05-06 10:11:12");
  REQUIRE(full[20].is_string());
  REQUIRE(full[20].get<std::string>().find("11:12") != std::string::npos);
  REQUIRE(full[21].get<std::string>().find("11:12") != std::string::npos);

  const auto &nulls = resp[DbResponseContract::kRows][1];
  for (const auto &value : nulls) {
    REQUIRE(value.is_null());
  }
}

TEST_CASE("PostgresQueryExecutor: positional parameter binding", "[db-live]") {
  REQUIRE_PG();
  PostgresQueryExecutor ex(make_pg_cfg(shared_pg()));
  REQUIRE(ex.init());

  int status = 0;
  const json params = {{"a", 7},      {"b", "str"}, {"c", true},
                       {"d", 3.5},    {"e", nullptr}};
  const json resp =
      ex.execute_query("SELECT $1::int AS i, $2::text AS t, $3::bool AS b, "
                       "$4::float8 AS n, $5::int AS v",
                       params, -1, 10, status);
  REQUIRE(status == 200);
  const auto &row = resp[DbResponseContract::kRows][0];
  REQUIRE(row[0] == 7);
  REQUIRE(row[1] == "str");
  REQUIRE(row[2] == true);
  REQUIRE(row[3] == 3.5);
  REQUIRE(row[4].is_null());
}

TEST_CASE("PostgresQueryExecutor: max_rows truncation and SQL errors",
          "[db-live]") {
  REQUIRE_PG();
  {
    PqConn conn(shared_pg());
    create_types_table(conn);
  }
  PostgresQueryExecutor ex(make_pg_cfg(shared_pg()));
  REQUIRE(ex.init());

  int status = 0;
  const json truncated = ex.execute_query(
      "SELECT c_i4 FROM pg_types", json::object(), -1, 1, status);
  REQUIRE(status == 200);
  REQUIRE(truncated[DbResponseContract::kRows].size() == 1);
  REQUIRE(truncated[DbResponseContract::kTruncated] == true);
  REQUIRE(truncated[DbResponseContract::kRowCount] == 1);

  int bad_status = 0;
  const json error = ex.execute_query("SELECT * FROM missing_table",
                                      json::object(), -1, 10, bad_status);
  REQUIRE(bad_status == 422);
  REQUIRE(error[DbResponseContract::kStatus] ==
          DbResponseContract::kStatusError);
  REQUIRE(error[DbResponseContract::kError][DbResponseContract::kCode] ==
          "SQL_ERROR");
}

TEST_CASE("PostgresQueryExecutor: statement timeout cancels slow queries",
          "[db-live]") {
  REQUIRE_PG();
  PostgresQueryExecutor ex(make_pg_cfg(shared_pg()));
  REQUIRE(ex.init());

  int status = 0;
  const json resp = ex.execute_query("SELECT pg_sleep(5), 1", json::object(),
                                     15, 1, status);
  REQUIRE(status == 422);
  REQUIRE(resp[DbResponseContract::kError][DbResponseContract::kCode] ==
          "SQL_ERROR");
  REQUIRE(ex.ping(1000));
}

TEST_CASE("PostgresQueryExecutor: pool exhaustion returns 503", "[db-live]") {
  REQUIRE_PG();
  PostgresQueryExecutor ex(make_pg_cfg(shared_pg(), /*pool_max=*/1));
  REQUIRE(ex.init());

  auto registry = std::make_shared<prometheus::Registry>();
  auto &capacity = test_support::make_gauge_family(
      *registry, "l2_worker_db_pool_capacity", "test");
  auto &exhausted = test_support::make_counter_family(
      *registry, "l2_worker_db_pool_exhausted_total", "test");
  auto &acquire = test_support::make_histogram_family(
      *registry, "l2_worker_db_pool_acquire_duration_seconds", "test");
  ex.set_pool_saturation_metrics(&capacity, &exhausted, &acquire);
  REQUIRE(capacity.Add({{"db", "pg"}}).Collect().gauge.value == 1.0);

  int slow_status = 0;
  std::thread slow([&ex, &slow_status] {
    json const body = ex.execute_query("SELECT pg_sleep(0.4), 1",
                                       json::object(), -1, 1, slow_status);
    (void)body;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  int status = 0;
  const json resp = ex.execute_query("SELECT 1", json::object(), -1, 1, status);
  REQUIRE(status == 503);
  REQUIRE(resp[DbResponseContract::kError][DbResponseContract::kCode] ==
          "DB_UNAVAILABLE");
  // The rejecting acquire is counted as pool exhaustion and timed (the slow
  // request's own acquire is observed too).
  REQUIRE(exhausted.Add({{"db", "pg"}}).Collect().counter.value == 1.0);
  REQUIRE(test_support::histogram_series(acquire, "pg")
              .Collect()
              .histogram.sample_count >= 2);
  slow.join();
}

TEST_CASE("PostgresQueryExecutor: zero-column result set is a SQL error",
          "[db-live]") {
  REQUIRE_PG();
  {
    // PostgreSQL accepts zero-column tables, and SELECT * FROM one yields
    // PGRES_TUPLES_OK with no fields at all.
    PqConn conn(shared_pg());
    conn.exec("CREATE TABLE IF NOT EXISTS pg_zero_col ()");
  }
  PostgresQueryExecutor ex(make_pg_cfg(shared_pg()));
  REQUIRE(ex.init());

  int status = 0;
  const json resp = ex.execute_query("SELECT * FROM pg_zero_col",
                                     json::object(), -1, 10, status);
  REQUIRE(status == 422);
  REQUIRE(resp[DbResponseContract::kStatus] ==
          DbResponseContract::kStatusError);
  REQUIRE(resp[DbResponseContract::kError][DbResponseContract::kMessage] ==
          "Statement returned no result set");
}

TEST_CASE("PostgresQueryExecutor: ping fails on a terminated backend",
          "[db-live]") {
  REQUIRE_PG();
  PostgresQueryExecutor ex(make_pg_cfg(shared_pg(), /*pool_max=*/1));
  REQUIRE(ex.init());

  int status = 0;
  REQUIRE(ex.execute_query("SELECT 1", json::object(), -1, 1, status)
              [DbResponseContract::kStatus] == DbResponseContract::kStatusOk);

  // Kill the backend behind the pooled connection: the connection object stays
  // alive locally, so the next command on it can no longer reach PostgreSQL.
  PqConn killer(shared_pg());
  killer.exec("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
              "WHERE datname = 'postgres' AND pid <> pg_backend_pid()");

  REQUIRE_FALSE(ex.ping(500));
  // The pool dropped the dead connection, so a fresh one is created and the
  // executor recovers on the next ping.
  REQUIRE(ex.ping(500));
}

TEST_CASE("PostgresQueryExecutor: handles a connection lost mid-pool",
          "[db-live]") {
  REQUIRE_PG();
  PostgresQueryExecutor ex(make_pg_cfg(shared_pg()));
  REQUIRE(ex.init());

  int status = 0;
  REQUIRE(ex.execute_query("SELECT 1", json::object(), -1, 1, status)
              [DbResponseContract::kStatus] == DbResponseContract::kStatusOk);

  shared_pg().stop();

  int dead_status = 0;
  const json resp =
      ex.execute_query("SELECT 1", json::object(), -1, 1, dead_status);
  REQUIRE(dead_status == 422);
  REQUIRE(resp[DbResponseContract::kError][DbResponseContract::kCode] ==
          "SQL_ERROR");
  REQUIRE_FALSE(ex.ping(500));

  REQUIRE_PG();
}

TEST_CASE("DbQueryHandler: routes live postgres query and ping", "[db-live]") {
  REQUIRE_PG();
  DbQueryHandler handler;
  auto registry = std::make_shared<prometheus::Registry>();
  auto &capacity = test_support::make_gauge_family(
      *registry, "l2_worker_db_pool_capacity", "test");
  auto &exhausted = test_support::make_counter_family(
      *registry, "l2_worker_db_pool_exhausted_total", "test");
  auto &acquire = test_support::make_histogram_family(
      *registry, "l2_worker_db_pool_acquire_duration_seconds", "test");
  handler.set_pool_metrics(nullptr);
  handler.set_pool_saturation_metrics(&capacity, &exhausted, &acquire);
  REQUIRE(handler.init({make_pg_cfg(shared_pg())}));
  REQUIRE(handler.is_enabled());
  REQUIRE(handler.all_configured());
  REQUIRE(handler.configured_databases() == std::vector<std::string>{"pg"});
  REQUIRE(handler.ready_databases() == std::vector<std::string>{"pg"});

  const json query{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
                   {DbQueryContract::kRequestId, "req-1"},
                   {DbQueryContract::kDb, "pg"},
                   {DbQueryContract::kSql, "SELECT 42 AS answer"}};
  int status = 0;
  json body;
  handler.handle_request(query, status, body);
  REQUIRE(status == 200);
  REQUIRE(body[DbResponseContract::kStatus] == DbResponseContract::kStatusOk);
  REQUIRE(body[DbResponseContract::kDb] == "pg");
  REQUIRE(body[DbResponseContract::kRows] ==
          json::array({json::array({42})}));

  const json implicit{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
                      {DbQueryContract::kRequestId, "req-2"},
                      {DbQueryContract::kSql, "SELECT 1 AS one"}};
  handler.handle_request(implicit, status, body);
  REQUIRE(status == 200);
  REQUIRE(body[DbResponseContract::kRows] ==
          json::array({json::array({1})}));

  const json ping{{DbQueryContract::kType, DbQueryContract::kTypePing},
                  {DbQueryContract::kRequestId, "req-3"},
                  {DbQueryContract::kDb, "pg"}};
  handler.handle_request(ping, status, body);
  REQUIRE(status == 200);
  REQUIRE(body[DbResponseContract::kDb] == "pg");
  REQUIRE(body.contains(DbResponseContract::kLatencyMs));

  const json unknown{{DbQueryContract::kType, DbQueryContract::kTypePing},
                     {DbQueryContract::kRequestId, "req-4"},
                     {DbQueryContract::kDb, "nope"}};
  handler.handle_request(unknown, status, body);
  REQUIRE(status == 404);
  REQUIRE(body[DbResponseContract::kError][DbResponseContract::kCode] ==
          "UNKNOWN_DATABASE");

  const json write{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
                   {DbQueryContract::kRequestId, "req-5"},
                   {DbQueryContract::kDb, "pg"},
                   {DbQueryContract::kSql, "INSERT INTO t VALUES (1)"}};
  handler.handle_request(write, status, body);
  REQUIRE(status == 400);
  REQUIRE(body[DbResponseContract::kError][DbResponseContract::kCode] ==
          "BAD_REQUEST");

  // The handler wired the saturation families into the executor it created:
  // capacity is the configured pool_max, no acquire was rejected, and every
  // acquire (query + ping) was timed.
  REQUIRE(capacity.Add({{"db", "pg"}}).Collect().gauge.value == 5.0);
  REQUIRE(exhausted.Add({{"db", "pg"}}).Collect().counter.value == 0.0);
  REQUIRE(test_support::histogram_series(acquire, "pg")
              .Collect()
              .histogram.sample_count >= 3);
}

TEST_CASE("DbQueryHandler: re-init keeps already ready executors", "[db-live]") {
  REQUIRE_PG();
  DbQueryHandler handler;
  REQUIRE(handler.init({make_pg_cfg(shared_pg())}));
  REQUIRE(handler.all_configured());
  REQUIRE(handler.ready_databases() == std::vector<std::string>{"pg"});

  // Second init with the same database name: the existing executor is kept
  // (the loop skips it) instead of being rebuilt.
  REQUIRE(handler.init({make_pg_cfg(shared_pg())}));
  REQUIRE(handler.ready_databases() == std::vector<std::string>{"pg"});

  const json query{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
                   {DbQueryContract::kRequestId, "req-reinit"},
                   {DbQueryContract::kDb, "pg"},
                   {DbQueryContract::kSql, "SELECT 9 AS x"}};
  int status = 0;
  json body;
  handler.handle_request(query, status, body);
  REQUIRE(status == 200);
  REQUIRE(body[DbResponseContract::kRows] == json::array({json::array({9})}));
}

TEST_CASE("DbQueryHandler: partial init leaves the reachable database ready",
          "[db-live]") {
  REQUIRE_PG();
  DbConfig const reachable = make_pg_cfg(shared_pg());
  DbConfig unreachable;
  unreachable.m_name = "other";
  unreachable.m_driver = "postgres";
  unreachable.m_host = "127.0.0.1";
  unreachable.m_port = 1;
  unreachable.m_database = "test";
  unreachable.m_user = "test";

  DbQueryHandler handler;
  REQUIRE(handler.init({reachable, unreachable}));
  REQUIRE(handler.is_enabled());
  REQUIRE_FALSE(handler.all_configured());
  REQUIRE(handler.configured_databases() ==
          std::vector<std::string>({"pg", "other"}));
  REQUIRE(handler.ready_databases() == std::vector<std::string>{"pg"});

  const json query{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
                   {DbQueryContract::kRequestId, "req-1"},
                   {DbQueryContract::kSql, "SELECT 7 AS x"}};
  int status = 0;
  json body;
  // Only 'pg' got an executor (the 'other' init failed), so the implicit
  // single-executor routing still serves it.
  handler.handle_request(query, status, body);
  REQUIRE(status == 200);
  REQUIRE(body[DbResponseContract::kRows] ==
          json::array({json::array({7})}));

  const json ping_other{{DbQueryContract::kType, DbQueryContract::kTypePing},
                        {DbQueryContract::kRequestId, "req-2"},
                        {DbQueryContract::kDb, "other"}};
  handler.handle_request(ping_other, status, body);
  REQUIRE(status == 404);
  REQUIRE(body[DbResponseContract::kError][DbResponseContract::kCode] ==
          "UNKNOWN_DATABASE");

  const json named{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
                   {DbQueryContract::kRequestId, "req-3"},
                   {DbQueryContract::kDb, "pg"},
                   {DbQueryContract::kSql, "SELECT 7 AS x"}};
  handler.handle_request(named, status, body);
  REQUIRE(status == 200);
  REQUIRE(body[DbResponseContract::kRows] ==
          json::array({json::array({7})}));
}

TEST_CASE("DbQueryHandler: empty db with multiple executors is 404",
          "[db-live]") {
  REQUIRE_PG();
  DbConfig const first = make_pg_cfg(shared_pg());
  DbConfig second = first;
  second.m_name = "pg2";

  DbQueryHandler handler;
  REQUIRE(handler.init({first, second}));
  REQUIRE(handler.is_enabled());
  REQUIRE(handler.all_configured());
  REQUIRE(handler.configured_databases() ==
          std::vector<std::string>({"pg", "pg2"}));
  REQUIRE(handler.ready_databases() ==
          std::vector<std::string>({"pg", "pg2"}));

  const json implicit{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
                      {DbQueryContract::kRequestId, "req-1"},
                      {DbQueryContract::kSql, "SELECT 7 AS x"}};
  int status = 0;
  json body;
  handler.handle_request(implicit, status, body);
  REQUIRE(status == 404);
  REQUIRE(body[DbResponseContract::kError][DbResponseContract::kCode] ==
          "UNKNOWN_DATABASE");

  const json named{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
                   {DbQueryContract::kRequestId, "req-2"},
                   {DbQueryContract::kDb, "pg"},
                   {DbQueryContract::kSql, "SELECT 7 AS x"}};
  handler.handle_request(named, status, body);
  REQUIRE(status == 200);
  REQUIRE(body[DbResponseContract::kRows] ==
          json::array({json::array({7})}));

  const json ping{{DbQueryContract::kType, DbQueryContract::kTypePing},
                  {DbQueryContract::kRequestId, "req-3"},
                  {DbQueryContract::kDb, "pg2"}};
  handler.handle_request(ping, status, body);
  REQUIRE(status == 200);
  REQUIRE(body[DbResponseContract::kDb] == "pg2");
}

TEST_CASE("DbQueryHandler: rejects malformed requests before dispatch",
          "[db]") {
  // No init()/live PG needed: parse_db_query_request fails before the executor
  // lookup, so these cover the DbQueryContract validation branches offline.
  DbQueryHandler handler;

  struct MalformedRequest {
    json m_request;
    std::string m_expect;
  };
  const std::vector<MalformedRequest> cases = {
      {json::array({1}), "must be a JSON object"},
      {json{{DbQueryContract::kType, "upsert"},
            {DbQueryContract::kRequestId, "r"},
            {DbQueryContract::kSql, "SELECT 1"}},
       "type must be"},
      {json{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
            {DbQueryContract::kRequestId, "r"}},
       "sql must be"},
      {json{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
            {DbQueryContract::kRequestId, "r"},
            {DbQueryContract::kSql, "DELETE FROM t"}},
       "only read-only"},
      {json{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
            {DbQueryContract::kRequestId, "r"},
            {DbQueryContract::kSql, "SELECT 1"},
            {DbQueryContract::kParams, json::array({1})}},
       "params must be an object"},
      {json{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
            {DbQueryContract::kRequestId, "r"},
            {DbQueryContract::kSql, "SELECT 1"},
            {DbQueryContract::kParams, json{{"x", json::array({1})}}}},
       "only scalar"},
      {json{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
            {DbQueryContract::kRequestId, "r"},
            {DbQueryContract::kSql, "SELECT 1"},
            {DbQueryContract::kTimeoutMs, 0}},
       "timeout_ms must be"},
      {json{{DbQueryContract::kType, DbQueryContract::kTypeQuery},
            {DbQueryContract::kRequestId, "r"},
            {DbQueryContract::kSql, "SELECT 1"},
            {DbQueryContract::kMaxRows, 0}},
       "max_rows must be"},
  };

  for (const auto &c : cases) {
    int status = 0;
    json body;
    handler.handle_request(c.m_request, status, body);
    REQUIRE(status == 400);
    REQUIRE(body[DbResponseContract::kStatus] ==
            DbResponseContract::kStatusError);
    const std::string message =
        body[DbResponseContract::kError][DbResponseContract::kMessage];
    CHECK(message.find(c.m_expect) != std::string::npos);
  }
}
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

#include "db_query_executor.hpp"
#include "db_query_executor_postgres.hpp"
#include "db_query_handler.hpp"
#include "db_query_utils.hpp"
#include "logger.hpp"

#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <grp.h>
#include <libpq-fe.h>
#include <memory>
#include <netinet/in.h>
#include <pwd.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr int kServerWaitTimeoutMs = 10000;

[[nodiscard]] int reserve_loopback_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
  }
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

[[nodiscard]] bool tcp_probe(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  const int rc = ::connect(fd, reinterpret_cast<const sockaddr *>(&addr),
                           sizeof(addr));
  ::close(fd);
  return rc == 0;
}

[[nodiscard]] bool wait_for_condition(const std::function<bool()> &predicate,
                                      int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return predicate();
}

// Unix user the server must run as: an existing non-root user (postgres, or the
// current user when the test already runs unprivileged). Root is refused by
// postgres.
struct TargetUser {
  bool m_ok = false;
  bool m_need_switch = false;
  uid_t m_uid = 0;
  gid_t m_gid = 0;
  std::string m_name;
};

[[nodiscard]] TargetUser resolve_target_user() {
  if (::geteuid() != 0) {
    return TargetUser{true, false, ::geteuid(), ::getegid(), ""};
  }
  const struct passwd *pw = ::getpwnam("postgres");
  if (pw == nullptr) {
    Logger::warn("pg live test: no 'postgres' user in this image, skipping");
    return TargetUser{};
  }
  return TargetUser{true,  true,  pw->pw_uid,
                    pw->pw_gid, std::string(pw->pw_name)};
}

[[nodiscard]] bool drop_privileges(const TargetUser &target) {
  if (!target.m_need_switch) {
    return true;
  }
  if (::setgid(target.m_gid) != 0) {
    return false;
  }
  if (::initgroups(target.m_name.c_str(), target.m_gid) != 0) {
    return false;
  }
  return ::setuid(target.m_uid) == 0;
}

[[nodiscard]] bool redirect_to(const char *path) {
  const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return false;
  }
  ::dup2(fd, 1);
  ::dup2(fd, 2);
  if (fd > 2) {
    ::close(fd);
  }
  return true;
}

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
  std::sort(versions.begin(), versions.end(),
            [](const std::string &a, const std::string &b) {
              return std::stoi(a) > std::stoi(b);
            });
  for (const std::string &version : versions) {
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
      return true;
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
  }

private:
  [[nodiscard]] bool alive() const {
    if (m_pid <= 0) {
      return false;
    }
    int status = 0;
    return ::waitpid(m_pid, &status, WNOHANG) == 0;
  }

  [[nodiscard]] bool run_wait(const std::string &exe,
                              const std::vector<std::string> &args,
                              const std::string &logfile,
                              const TargetUser &target) const {
    const pid_t pid = ::fork();
    if (pid == 0) {
      if (!drop_privileges(target)) {
        _exit(127);
      }
      redirect_to(logfile.c_str());
      std::vector<const char *> argv;
      argv.reserve(args.size() + 2);
      argv.push_back(exe.c_str());
      for (const std::string &arg : args) {
        argv.push_back(arg.c_str());
      }
      argv.push_back(nullptr);
      ::execv(exe.c_str(), const_cast<char *const *>(argv.data()));
      _exit(127);
    }
    if (pid < 0) {
      Logger::warn("pg live test: fork failed for {}", exe);
      return false;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      Logger::warn("pg live test: '{}' failed (see {})", exe, logfile);
      return false;
    }
    return true;
  }

  [[nodiscard]] pid_t spawn_server(const std::string &exe,
                                   const std::vector<std::string> &args,
                                   const std::string &logfile,
                                   const TargetUser &target) const {
    const pid_t pid = ::fork();
    if (pid == 0) {
      if (!drop_privileges(target)) {
        _exit(127);
      }
      redirect_to(logfile.c_str());
      std::vector<const char *> argv;
      argv.reserve(args.size() + 2);
      argv.push_back(exe.c_str());
      for (const std::string &arg : args) {
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
    const TargetUser target = resolve_target_user();
    if (!target.m_ok) {
      return false;
    }

    m_port = reserve_loopback_port();
    if (m_port <= 0) {
      Logger::warn("pg live test: no free loopback port");
      return false;
    }

    const std::string tmpl = "/tmp/pgdb-XXXXXX";
    std::vector<char> dir_tmpl(tmpl.begin(), tmpl.end());
    dir_tmpl.push_back('\0');
    char *dir = ::mkdtemp(dir_tmpl.data());
    if (dir == nullptr) {
      Logger::warn("pg live test: mkdtemp failed");
      return false;
    }
    m_datadir = dir;

    if (target.m_need_switch &&
        ::chown(m_datadir.c_str(), target.m_uid, target.m_gid) != 0) {
      Logger::warn("pg live test: chown datadir failed");
      cleanup_datadir();
      return false;
    }

    const std::string init_log = std::format("/tmp/pg-init-{}.log", m_port);
    if (!run_wait(m_bin_dir + "/initdb",
                  {"-D", m_datadir, "-U", "postgres", "-A", "trust",
                   "--no-locale", "-E", "UTF8"},
                  init_log, target)) {
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
      Logger::warn("pg live test: fork postgres failed");
      cleanup_datadir();
      return false;
    }
    m_pid = server_pid;

    const bool ready = wait_for_condition(
        [this] { return tcp_probe(m_port) && alive(); },
        kServerWaitTimeoutMs);
    if (!ready) {
      Logger::warn("pg live test: server at {} never became ready", m_port);
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
            "'\\x0102'::bytea, '{\"a\":1}'::json, '{\"b\":2}'::jsonb, "
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

  const json &cols = resp[DbResponseContract::kColumns];
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

  const json &full = resp[DbResponseContract::kRows][0];
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
  REQUIRE(full[13] == "AQI=");
  REQUIRE(full[16] == "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11");
  REQUIRE(full[17] == "2024-05-06");
  REQUIRE(full[18] == "10:11:12");
  REQUIRE(full[19] == "2024-05-06 10:11:12");
  REQUIRE(full[20].is_string());
  REQUIRE(full[20].get<std::string>().find("11:12") != std::string::npos);
  REQUIRE(full[21].get<std::string>().find("11:12") != std::string::npos);

  const json &nulls = resp[DbResponseContract::kRows][1];
  for (const json &value : nulls) {
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
  const json &row = resp[DbResponseContract::kRows][0];
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

  int slow_status = 0;
  std::thread slow([&ex, &slow_status] {
    json body = ex.execute_query("SELECT pg_sleep(0.4), 1", json::object(), -1,
                                 1, slow_status);
    (void)body;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  int status = 0;
  const json resp = ex.execute_query("SELECT 1", json::object(), -1, 1, status);
  REQUIRE(status == 503);
  REQUIRE(resp[DbResponseContract::kError][DbResponseContract::kCode] ==
          "DB_UNAVAILABLE");
  slow.join();
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
}

TEST_CASE("DbQueryHandler: partial init leaves the reachable database ready",
          "[db-live]") {
  REQUIRE_PG();
  DbConfig reachable = make_pg_cfg(shared_pg());
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
  DbConfig first = make_pg_cfg(shared_pg());
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
#include "app_context.hpp"
#include "config.hpp"
#include "db_spider.hpp"
#include "ini_parser.hpp"
#include "json_utils.hpp"
#include "nlohmann/json.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <prometheus/counter.h>
#include <string>

namespace {

// RAII helper to set/restore an env var for the duration of a test.
struct EnvGuard {
  explicit EnvGuard(const std::string &name, const std::string &value)
      : m_name(name) {
    const char *old = std::getenv(name.c_str());
    m_old_set = old != nullptr;
    if (m_old_set) {
      m_old_value = old;
    }
    setenv(name.c_str(), value.c_str(), 1);
  }
  ~EnvGuard() {
    if (m_old_set) {
      setenv(m_name.c_str(), m_old_value.c_str(), 1);
    } else {
      unsetenv(m_name.c_str());
    }
  }
  std::string m_name;
  std::string m_old_value;
  bool m_old_set = false;
};

// Writes a temp INI file and returns its path. Cleaned up by RAII.
struct IniFileGuard {
  explicit IniFileGuard(const std::string &content) {
    m_path = "/tmp/spider-test-" + std::to_string(++counter) + ".ini";
    std::ofstream out(m_path);
    out << content;
  }
  ~IniFileGuard() { std::filesystem::remove(m_path); }
  std::string m_path;
  static int counter;
};

int IniFileGuard::counter = 0;

} // namespace

TEST_CASE("DB spider: parses INI config with databases and queries") {
  const std::string ini = R"(
; l2-db-spider config
[spider]
interval_seconds = 600
output_dir = /tmp/spider-out

[db:ccm3t]
driver = oracle
host = ccm3t.nlmk
port = 1521
service = FIPC_NSI
user = app_user
password = secret
pool_min = 1
pool_max = 3

[query:shared_pool]
db = ccm3t
sql = SELECT * FROM v$sgastat
timeout_ms = 5000

[query:table_sizes]
db = ccm3t
sql = SELECT segment_name, bytes FROM user_segments

[db:pg1]
driver = postgres
host = postgres
port = 5432
database = app_db
user = app_user
password = secret

[query:pg_table_sizes]
db = pg1
sql = SELECT relname, pg_total_relation_size(relid) AS size FROM pg_class
)";
  const IniFileGuard ini_guard(ini);
  const EnvGuard guard1("DB_SPIDER_ENABLED", "true");
  const EnvGuard guard2("DB_SPIDER_CONFIG_FILE", ini_guard.m_path);
  const EnvGuard guard3("DB_SPIDER_INTERVAL_SECONDS", "300");

  Config cfg;
  cfg.load_from_env();

  REQUIRE(cfg.m_spider.m_enabled == true);
  REQUIRE(cfg.m_spider.m_config_file == ini_guard.m_path);
  // [spider] section overrides the env default.
  REQUIRE(cfg.m_spider.m_interval_seconds == 600);
  REQUIRE(cfg.m_spider.m_output_dir == "/tmp/spider-out");
  REQUIRE(cfg.m_spider.m_databases.size() == 2);

  const auto &oracle = cfg.m_spider.m_databases[0];
  REQUIRE(oracle.m_name == "ccm3t");
  REQUIRE(oracle.m_driver == "oracle");
  REQUIRE(oracle.m_host == "ccm3t.nlmk");
  REQUIRE(oracle.m_port == 1521);
  REQUIRE(oracle.m_service == "FIPC_NSI");
  REQUIRE(oracle.m_pool_max == 3);
  REQUIRE(oracle.m_queries.size() == 2);
  REQUIRE(oracle.m_queries[0].m_name == "shared_pool");
  REQUIRE(oracle.m_queries[0].m_sql == "SELECT * FROM v$sgastat");

  const auto &pg = cfg.m_spider.m_databases[1];
  REQUIRE(pg.m_name == "pg1");
  REQUIRE(pg.m_driver == "postgres");
  REQUIRE(pg.m_database == "app_db");
  REQUIRE(pg.m_queries.size() == 1);
  REQUIRE(pg.m_queries[0].m_name == "pg_table_sizes");
}

TEST_CASE("DB spider: missing config file yields no databases") {
  const EnvGuard guard1("DB_SPIDER_ENABLED", "true");
  const EnvGuard guard2("DB_SPIDER_CONFIG_FILE", "/tmp/does-not-exist.ini");
  const EnvGuard guard3("DB_SPIDER_OUTPUT_DIR", "/tmp/spider");

  Config cfg;
  cfg.load_from_env();
  REQUIRE(cfg.m_spider.m_enabled == true);
  REQUIRE(cfg.m_spider.m_databases.empty());
}

TEST_CASE("DB spider: validation rejects invalid database") {
  const std::string ini = R"(
[db:bad]
driver = oracle
host =
port = 0
service =
user =
)";
  const IniFileGuard ini_guard(ini);
  const EnvGuard guard1("DB_SPIDER_ENABLED", "true");
  const EnvGuard guard2("DB_SPIDER_CONFIG_FILE", ini_guard.m_path);
  const EnvGuard guard3("DB_SPIDER_OUTPUT_DIR", "/tmp/spider");

  Config cfg;
  cfg.load_from_env();
  // Missing host/service/user/port must make validation fail.
  REQUIRE_FALSE(cfg.validate(false));
}

TEST_CASE("DB spider: query referencing unknown db is skipped") {
  const std::string ini = R"(
[db:real]
driver = postgres
host = h
port = 5432
database = d
user = u
password = p

[query:orphan]
db = missing
sql = SELECT 1
)";
  const IniFileGuard ini_guard(ini);
  const EnvGuard guard1("DB_SPIDER_ENABLED", "true");
  const EnvGuard guard2("DB_SPIDER_CONFIG_FILE", ini_guard.m_path);

  Config cfg;
  cfg.load_from_env();
  REQUIRE(cfg.m_spider.m_databases.size() == 1);
  REQUIRE(cfg.m_spider.m_databases[0].m_queries.empty());
}

TEST_CASE("INI parser: sections, keys, comments, case-insensitivity") {
  const std::string ini = R"(
; comment
# another comment

[spider]
interval_seconds = 300
output_dir = /tmp

[db:ccm3t]
driver = oracle
)";
  std::vector<IniParser::Section> sections;
  std::string error;
  const IniFileGuard ini_guard(ini);
  REQUIRE(IniParser::parse_file(ini_guard.m_path, sections, error));
  REQUIRE(sections.size() == 2);

  const auto *spider = IniParser::find_section(sections, "SPIDER");
  REQUIRE(spider != nullptr);
  REQUIRE(IniParser::get_key(*spider, "interval_seconds") == std::string("300"));
  REQUIRE(IniParser::get_key(*spider, "OUTPUT_DIR") == std::string("/tmp"));

  const auto *db = IniParser::find_section(sections, "db:ccm3t");
  REQUIRE(db != nullptr);
  REQUIRE(IniParser::get_key(*db, "driver") == std::string("oracle"));
}

TEST_CASE("INI parser: missing file returns error") {
  std::vector<IniParser::Section> sections;
  std::string error;
  REQUIRE_FALSE(IniParser::parse_file("/tmp/no-such-file.ini", sections, error));
  REQUIRE_FALSE(error.empty());
}

TEST_CASE("DB spider: run_poll writes error JSON for a database without an "
          "executor") {
  const std::string ini = R"(
[spider]
interval_seconds = 0

[db:pg-one]
driver = postgres
host = 127.0.0.1
port = 15432
database = d
user = u
password = p

[query:q:one]
db = pg-one
sql = SELECT 1
timeout_ms = 1000

[query:q-two]
db = pg-one
sql = SELECT 2
)";
  const IniFileGuard ini_guard(ini);
  const std::string out_dir =
      "/tmp/spider-test-out-" + std::to_string(++IniFileGuard::counter);
  const EnvGuard guard1("DB_SPIDER_ENABLED", "true");
  const EnvGuard guard2("DB_SPIDER_CONFIG_FILE", ini_guard.m_path);
  const EnvGuard guard3("DB_SPIDER_OUTPUT_DIR", out_dir);
  const EnvGuard guard4("DB_SPIDER_INTERVAL_SECONDS", "0");

  AppContext ctx;
  REQUIRE(ctx.m_config.m_spider.m_databases.size() == 1);
  // Mutate after validation: the factory now refuses the driver, so the
  // handler has no executor and stream_output takes its documented error path.
  ctx.m_config.m_spider.m_databases[0].m_driver = "unsupported";

  DbSpider spider(ctx);
  json const result = spider.run_poll();

  REQUIRE(result.contains("databases"));
  REQUIRE(result["databases"].size() == 1);
  REQUIRE(result["databases"][0]["db"] == "pg-one");
  REQUIRE(result["databases"][0]["driver"] == "unsupported");
  auto const &queries = result["databases"][0]["queries"];
  REQUIRE(queries.size() == 2);
  REQUIRE(queries["q:one"]["status"] == "error");
  REQUIRE(queries["q-two"]["status"] == "error");

  // Names with punctuation are sanitized into file-name-safe segments.
  std::ifstream in(out_dir + "/pg-one_q_one.json");
  REQUIRE(in.is_open());
  json file;
  in >> file;
  REQUIRE(file["db"] == "pg-one");
  REQUIRE(file["query"] == "q:one");
  REQUIRE(file["status"] == "error");
  REQUIRE(file["result"]["error"]["code"] == 404);
  REQUIRE(std::filesystem::exists(out_dir + "/pg-one_q-two.json"));

  auto &metric = ctx.m_spider.m_metrics->m_queries_total.Add(
      {{"db", "pg-one"}, {"query", "q:one"}, {"status", "error"}});
  REQUIRE(metric.Value() == 1);
  std::filesystem::remove_all(out_dir);
}

TEST_CASE("DB spider: unwritable output directory is reported as an error") {
  const std::string ini = R"(
[db:pg]
driver = postgres
host = 127.0.0.1
port = 15432
database = d
user = u
password = p

[query:q]
db = pg
sql = SELECT 1
)";
  const IniFileGuard ini_guard(ini);
  const std::string blocked =
      "/tmp/spider-test-blocked-" + std::to_string(++IniFileGuard::counter);
  {
    std::ofstream out(blocked);
    out << "a regular file, not a directory";
  }
  const EnvGuard guard1("DB_SPIDER_ENABLED", "true");
  const EnvGuard guard2("DB_SPIDER_CONFIG_FILE", ini_guard.m_path);
  const EnvGuard guard3("DB_SPIDER_OUTPUT_DIR", blocked);

  AppContext ctx;
  ctx.m_config.m_spider.m_databases[0].m_driver = "unsupported";

  DbSpider spider(ctx);
  json const result = spider.run_poll();

  REQUIRE(result["databases"][0]["queries"].size() == 1);
  REQUIRE(result["databases"][0]["queries"]["q"]["status"] == "error");
  std::filesystem::remove(blocked);
}

TEST_CASE("DB spider: config supports name override, sql_file and env "
          "placeholders") {
  const std::string sql_path =
      "/tmp/spider-test-" + std::to_string(++IniFileGuard::counter) + ".sql";
  {
    std::ofstream out(sql_path);
    out << "SELECT 42";
  }
  const std::string ini = R"(
[spider]
interval_seconds = 0
output_dir = /tmp/spider-out

[db:pg]
driver = postgres
host = 127.0.0.1
port = 15432
database = d
user = %SPIDER_TEST_USER%
password = %SPIDER_TEST_PASS%

[query:section_name]
name = friendly
db = pg
sql_file = )" + sql_path + R"(
timeout_ms = 250
max_rows = 7
)";
  const IniFileGuard ini_guard(ini);
  const EnvGuard guard1("DB_SPIDER_ENABLED", "true");
  const EnvGuard guard2("DB_SPIDER_CONFIG_FILE", ini_guard.m_path);
  const EnvGuard guard3("SPIDER_TEST_USER", "env_user");
  const EnvGuard guard4("SPIDER_TEST_PASS", "env_pw");

  Config cfg;
  cfg.load_from_env();

  REQUIRE(cfg.m_spider.m_databases.size() == 1);
  auto const &db = cfg.m_spider.m_databases[0];
  REQUIRE(db.m_user == "env_user");
  REQUIRE(db.m_password == "env_pw");
  REQUIRE(db.m_queries.size() == 1);
  REQUIRE(db.m_queries[0].m_name == "friendly");
  REQUIRE(db.m_queries[0].m_sql == "SELECT 42");
  REQUIRE(db.m_queries[0].m_timeout_ms == 250);
  REQUIRE(db.m_queries[0].m_max_rows == 7);
  std::filesystem::remove(sql_path);
}

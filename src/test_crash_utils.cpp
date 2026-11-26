// Unit tests for the crash-handler utility helpers (crash_utils.hpp): signal
// name mapping, line trimming, C++ symbol demangling and /proc/self/exe path.

#include "crash_dsn.hpp"
#include "crash_dump_counter.hpp"
#include "crash_utils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <string>
#include <string_view>
#include <unistd.h>

TEST_CASE("CrashUtils: signal_name maps known and unknown signals",
          "[crash-utils]") {
  REQUIRE(std::string(crash_utils::signal_name(SIGSEGV)) == "SIGSEGV");
  REQUIRE(std::string(crash_utils::signal_name(SIGABRT)) == "SIGABRT");
  REQUIRE(std::string(crash_utils::signal_name(SIGFPE)) == "SIGFPE");
  REQUIRE(std::string(crash_utils::signal_name(SIGBUS)) == "SIGBUS");
  REQUIRE(std::string(crash_utils::signal_name(SIGILL)) == "SIGILL");
  REQUIRE(std::string(crash_utils::signal_name(42)) == "UNKNOWN");
}

TEST_CASE("CrashUtils: trim_line strips trailing line breaks", "[crash-utils]") {
  REQUIRE(crash_utils::trim_line("value\n") == "value");
  REQUIRE(crash_utils::trim_line("value\r\n") == "value");
  REQUIRE(crash_utils::trim_line("value\n\n") == "value");
  REQUIRE(crash_utils::trim_line("value") == "value");
  REQUIRE(crash_utils::trim_line("") == "");
  REQUIRE(crash_utils::trim_line("\n") == "");
  REQUIRE(crash_utils::trim_line("line1\nline2") == "line1\nline2");
}

TEST_CASE("CrashUtils: demangle_symbol decodes mangled names", "[crash-utils]") {
  const std::string demangled =
      crash_utils::demangle_symbol("_ZN3foo3barEv");
  REQUIRE(demangled == "foo::bar()");

  REQUIRE(crash_utils::demangle_symbol("plain_symbol") == "plain_symbol");
  REQUIRE(crash_utils::demangle_symbol(nullptr) == "");
  REQUIRE(crash_utils::demangle_symbol("") == "");
}

TEST_CASE("CrashUtils: self_exe_path resolves a non-empty binary path",
          "[crash-utils]") {
  const std::string exe = crash_utils::self_exe_path();
  REQUIRE_FALSE(exe.empty());
  REQUIRE(exe.front() == '/'); // absolute path from readlink(/proc/self/exe)
  REQUIRE(exe.find('/') != std::string::npos);
}
TEST_CASE("CrashDumpCounter: counts dump files and ignores everything else",
          "[crash-utils]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("l2_crash_dumps_test_" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  // No directory yet: a fresh container has written no dumps.
  REQUIRE(crash_dump_counter::count_files((dir / "missing").string()) == 0);
  REQUIRE(crash_dump_counter::count_files(dir.string()) == 0);

  for (const char *name : {"crash_20260101_120000_SIGSEGV.txt",
                           "crash_20260101_120100_SIGABRT.txt",
                           "crash_test_marker.txt", "notes.txt",
                           "crash_20260101_120200_SIGSEGV.log"}) {
    std::ofstream const file(dir / name);
    REQUIRE(file.is_open());
  }

  REQUIRE(crash_dump_counter::count_files(dir.string()) == 2);
  REQUIRE(crash_dump_counter::is_dump_file_name(
      "crash_20260101_120000_SIGSEGV.txt"));
  REQUIRE_FALSE(crash_dump_counter::is_dump_file_name("crash_test_marker.txt"));
  REQUIRE_FALSE(crash_dump_counter::is_dump_file_name("crash_.txt"));

  std::filesystem::remove_all(dir);
}

TEST_CASE("CrashDsn: parses scheme, key, host, port and project",
          "[crash-utils]") {
  {
    const auto parsed = crash_dsn::parse_sentry_dsn("https://key@host/42");
    REQUIRE(parsed.m_dsn_raw == "https://key@host/42");
    REQUIRE(parsed.m_key == "key");
    REQUIRE(parsed.m_host == "host");
    REQUIRE(parsed.m_port == 443);
    REQUIRE(parsed.m_project == "42");
  }
  {
    const auto parsed =
        crash_dsn::parse_sentry_dsn("http://public@sentry.local:8080/7");
    REQUIRE(parsed.m_key == "public");
    REQUIRE(parsed.m_host == "sentry.local");
    REQUIRE(parsed.m_port == 8080);
    REQUIRE(parsed.m_project == "7");
  }
  {
    // No project part: the host still parses, project stays empty.
    const auto parsed = crash_dsn::parse_sentry_dsn("http://key@host");
    REQUIRE(parsed.m_host == "host");
    REQUIRE(parsed.m_port == 80);
    REQUIRE(parsed.m_project.empty());
  }
}

TEST_CASE("CrashDsn: a malformed port never throws and falls back",
          "[crash-utils]") {
  for (const char *dsn :
       {"https://key@host:notaport/42", "https://key@host:0/42",
        "https://key@host:65536/42", "https://key@host:/42",
        "http://key@host:99999999999999999999/42"}) {
    const auto parsed = crash_dsn::parse_sentry_dsn(dsn);
    REQUIRE(parsed.m_host == "host");
    REQUIRE(parsed.m_port == (std::string_view(dsn).starts_with("https")
                                  ? 443
                                  : 80));
  }
}

TEST_CASE("CrashDsn: an incomplete DSN disables Sentry without throwing",
          "[crash-utils]") {
  for (const char *dsn : {"", "not-a-dsn", "https://key/42", "https:///42"}) {
    const auto parsed = crash_dsn::parse_sentry_dsn(dsn);
    REQUIRE(parsed.m_host.empty());
    REQUIRE(parsed.m_key.empty());
    REQUIRE(parsed.m_project.empty());
  }
}

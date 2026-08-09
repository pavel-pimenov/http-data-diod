#ifndef CRASH_HANDLER_HPP
#define CRASH_HANDLER_HPP

#include "logger.hpp"
#include <array>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <sys/stat.h>
#include <unistd.h>

// Crash dump directory (overridable via CRASH_DUMP_DIR env var)
constexpr const char *g_default_crash_dump_dir = "/crash-dumps";

class CrashHandler {
public:
  static void install(const std::string &dump_dir = g_default_crash_dump_dir) {
    s_dump_dir = dump_dir;
    mkdir(s_dump_dir.c_str(), 0755);

    struct sigaction sa{};
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);

    Logger::info("Crash handler installed, dumps will be written to {}",
                 s_dump_dir);
  }

private:
  static inline std::string s_dump_dir = g_default_crash_dump_dir;

  static const char *signal_name(int signum) {
    switch (signum) {
    case SIGSEGV:
      return "SIGSEGV";
    case SIGABRT:
      return "SIGABRT";
    case SIGFPE:
      return "SIGFPE";
    case SIGBUS:
      return "SIGBUS";
    case SIGILL:
      return "SIGILL";
    default:
      return "UNKNOWN";
    }
  }

  // async-signal-safe: only uses POSIX open/write/close and stack-allocated
  // buffers. No heap allocation, no iostream — a signal may interrupt malloc.
  static void write_crash_report(int signum, siginfo_t *info) {
    // Generate timestamped filename using stack buffer
    time_t now = time(nullptr);
    struct tm tm_buf{};
    localtime_r(&now, &tm_buf);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &tm_buf);

    char filename[512];
    const char *dir = s_dump_dir.c_str();
    const char *sig = signal_name(signum);
    // Build filename: dir/crash_YYYYMMDD_HHMMSS_SIGNAL.txt
    int pos = 0;
    auto append = [&](const char *s) {
      while (*s && pos < static_cast<int>(sizeof(filename)) - 1) {
        filename[pos++] = *s++;
      }
    };
    append(dir);
    append("/crash_");
    append(time_str);
    append("_");
    append(sig);
    append(".txt");
    filename[pos] = '\0';

    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      return;
    }

    // Write header
    const char *header1 = "=== CRASH REPORT ===\n";
    const char *header2 = "Signal: ";
    const char *pid_prefix = "\nPID: ";
    const char *ts_prefix = "\nTimestamp: ";
    write(fd, header1, strlen(header1));
    write(fd, header2, strlen(header2));
    write(fd, sig, strlen(sig));
    write(fd, pid_prefix, strlen(pid_prefix));

    // PID
    char pid_buf[16];
    int pid_len = 0;
    pid_t pid = getpid();
    // itoa for PID
    if (pid == 0) {
      pid_buf[pid_len++] = '0';
    } else {
      char tmp[16];
      int i = 0;
      while (pid > 0) {
        tmp[i++] = '0' + (pid % 10);
        pid /= 10;
      }
      while (i > 0) {
        pid_buf[pid_len++] = tmp[--i];
      }
    }
    pid_buf[pid_len] = '\0';
    write(fd, pid_buf, pid_len);

    // Timestamp
    write(fd, ts_prefix, strlen(ts_prefix));
    write(fd, time_str, strlen(time_str));
    write(fd, "\n", 1);

    // Fault address
    if (info != nullptr) {
      const char *fault_prefix = "Fault address: ";
      write(fd, fault_prefix, strlen(fault_prefix));
      char addr_buf[32];
      snprintf(addr_buf, sizeof(addr_buf), "%p", info->si_addr);
      write(fd, addr_buf, strlen(addr_buf));
      write(fd, "\n", 1);
    }

    // Stack trace using backtrace() — write raw addresses for post-mortem
    // resolution (backtrace_symbols() allocates, so only raw addresses here)
    const char *stack_header = "\n=== STACK TRACE (raw addresses) ===\n";
    write(fd, stack_header, strlen(stack_header));

    void *callstack[128];
    int frames = backtrace(callstack, 128);

    for (int i = 0; i < frames; ++i) {
      char frame_buf[64];
      snprintf(frame_buf, sizeof(frame_buf), "#%d %p\n", i, callstack[i]);
      write(fd, frame_buf, strlen(frame_buf));
    }

    const char *resolve_hint =
        "\nResolve with: addr2line -e ./l2-proxy -fC <address>\n"
        "Or run: scripts/resolve-crash.sh <dump_file>\n";
    write(fd, resolve_hint, strlen(resolve_hint));

    close(fd);
  }

  static void signal_handler(int signum, siginfo_t *info, void * /*context*/) {
    write_crash_report(signum, info);

    // Re-raise with default handler to generate core dump
    signal(signum, SIG_DFL);
    raise(signum);
  }
};

#endif // CRASH_HANDLER_HPP

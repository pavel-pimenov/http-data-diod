#ifndef CRASH_HANDLER_HPP
#define CRASH_HANDLER_HPP

#include "logger.hpp"
#include "l2-proxy-version.h"
#include "nlohmann/json.hpp"
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <netdb.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#if __has_include(<stacktrace>)
#include <print>
#include <stacktrace>
#endif

// Crash dump directory (overridable via CRASH_DUMP_DIR env var)
constexpr const char *g_default_crash_dump_dir = "/crash-dumps";

extern const char *g_l2_proxy_version;

class CrashHandler {
public:
  static void install(const std::string &dump_dir = g_default_crash_dump_dir,
                      const std::string &sentry_dsn = {}) {
    m_dump_dir = dump_dir;
    mkdir(m_dump_dir.c_str(), 0755);

    if (!sentry_dsn.empty()) {
      parse_sentry_dsn(sentry_dsn);
    }

    struct sigaction sa{};
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);

    Logger::info("Crash handler installed, dumps will be written to {}{}",
                 m_dump_dir,
                 m_sentry_project.empty() ? "" : " (Sentry enabled)");
  }

private:
  static inline std::string m_dump_dir = g_default_crash_dump_dir;
  static inline std::string m_sentry_dsn_raw;
  static inline std::string m_sentry_host;
  static inline uint16_t m_sentry_port = 0;
  static inline std::string m_sentry_key;
  static inline std::string m_sentry_project;
  static inline constexpr int kMaxFrames = 128;
  static inline void *m_crash_frames[kMaxFrames];
  static inline int m_crash_frame_count = 0;

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

  static void parse_sentry_dsn(const std::string &dsn) {
    m_sentry_dsn_raw = dsn;
    auto scheme_end = dsn.find("://");
    if (scheme_end == std::string::npos) {
      return;
    }
    size_t start = scheme_end + 3;
    auto at_pos = dsn.find('@', start);
    if (at_pos == std::string::npos) {
      return;
    }
    m_sentry_key = dsn.substr(start, at_pos - start);
    size_t host_start = at_pos + 1;
    auto slash_pos = dsn.find('/', host_start);
    std::string host_port;
    if (slash_pos == std::string::npos) {
      host_port = dsn.substr(host_start);
    } else {
      host_port = dsn.substr(host_start, slash_pos - host_start);
    }
    auto colon = host_port.find(':');
    if (colon != std::string::npos) {
      m_sentry_host = host_port.substr(0, colon);
      m_sentry_port = static_cast<uint16_t>(
          std::stoi(host_port.substr(colon + 1)));
    } else {
      m_sentry_host = host_port;
      m_sentry_port =
          (dsn.substr(0, scheme_end) == "https") ? 443 : 80;
    }
    if (slash_pos != std::string::npos) {
      m_sentry_project = dsn.substr(slash_pos + 1);
    }
  }

  // async-signal-safe: only uses POSIX open/write/close and stack-allocated
  // buffers. No heap allocation, no iostream — a signal may interrupt malloc.
  static void write_crash_report(int signum, const siginfo_t *info) {
    // Generate timestamped filename using stack buffer
    time_t now = time(nullptr);
    struct tm tm_buf{};
    localtime_r(&now, &tm_buf);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &tm_buf);

    char filename[512];
    const char *dir = m_dump_dir.c_str();
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

  static std::string demangle_symbol(const char *mangled) {
    int status = 0;
    char *demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    if (status == 0 && demangled != nullptr) {
      std::string res(demangled);
      std::free(demangled);
      return res;
    }
    return mangled != nullptr ? mangled : "";
  }

  static std::string describe_frame(const void *addr) {
    Dl_info dli{};
    char buf[64];
    if (dladdr(addr, &dli) != 0 && dli.dli_sname != nullptr) {
      std::string name = demangle_symbol(dli.dli_sname);
      if (dli.dli_saddr != nullptr) {
        const unsigned long off = reinterpret_cast<unsigned long>(addr) -
                                  reinterpret_cast<unsigned long>(dli.dli_saddr);
        snprintf(buf, sizeof(buf), " (+0x%lx)", off);
        name += buf;
      }
      return name;
    }
    snprintf(buf, sizeof(buf), "0x%lx", reinterpret_cast<unsigned long>(addr));
    return buf;
  }

  static int find_faulting_frame_index() {
    for (int i = 1; i < m_crash_frame_count; ++i) {
      Dl_info dli{};
      if (dladdr(m_crash_frames[i], &dli) != 0 && dli.dli_sname != nullptr) {
        return i;
      }
    }
    return m_crash_frame_count > 1 ? 1 : 0;
  }

  static std::string trim_line(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
      s.pop_back();
    }
    return s;
  }

  static std::string self_exe_path() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
      return {};
    }
    buf[n] = '\0';
    return buf;
  }

  static std::string base_name(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  // Annotate frames that belong to the main executable with src file:line
  // resolved via addr2line (DWARF present because we link RelWithDebInfo).
  // Runs in the forked crash child, so fork/exec and tmpfile are safe.
  static void annotate_exe_frames(nlohmann::json &frames) {
    constexpr const char *kAddr2line = "/usr/bin/addr2line";
    if (access(kAddr2line, X_OK) != 0) {
      return;
    }
    const std::string exe = self_exe_path();
    if (exe.empty()) {
      return;
    }
    const std::string exe_base = base_name(exe);

    std::vector<size_t> exe_idx;
    for (size_t i = 0; i < frames.size(); ++i) {
      Dl_info dli{};
      if (dladdr(m_crash_frames[i], &dli) == 0 || dli.dli_sname == nullptr) {
        continue;
      }
      const std::string fname =
          dli.dli_fname != nullptr ? dli.dli_fname : "";
      if (base_name(fname) != exe_base) {
        continue;
      }
      exe_idx.push_back(i);
    }
    if (exe_idx.empty()) {
      return;
    }

    char in_path[256];
    char out_path[256];
    snprintf(in_path, sizeof(in_path), "%s/addr2line-in-%d.txt", m_dump_dir.c_str(), static_cast<int>(getpid()));
    snprintf(out_path, sizeof(out_path), "%s/addr2line-out-%d.txt", m_dump_dir.c_str(), static_cast<int>(getpid()));
    FILE *wf = fopen(in_path, "w");
    if (wf == nullptr) {
      return;
    }
    for (const size_t i : exe_idx) {
      Dl_info dli{};
      if (dladdr(m_crash_frames[i], &dli) != 0) {
        const unsigned long off =
            reinterpret_cast<unsigned long>(m_crash_frames[i]) -
            reinterpret_cast<unsigned long>(dli.dli_fbase);
        fprintf(wf, "0x%lx\n", off);
      }
    }
    fclose(wf);

    const std::string cmd = std::string(kAddr2line) + " -e '" + exe +
                            "' -f -C < '" + in_path + "' > '" + out_path +
                            "' 2>/dev/null";
    static_cast<void>(system(cmd.c_str()));
    unlink(in_path);

    FILE *in = fopen(out_path, "r");
    unlink(out_path);
    if (in == nullptr) {
      return;
    }
    std::vector<std::string> lines;
    char line[512];
    while (fgets(line, sizeof(line), in) != nullptr) {
      lines.emplace_back(line);
    }
    fclose(in);

    // addr2line -f -C emits 2 lines per input address: function, file:line
    for (size_t k = 0; k < exe_idx.size(); ++k) {
      const size_t base = 2 * k;
      if (base + 1 >= lines.size()) {
        break;
      }
      const std::string func = trim_line(lines[base]);
      const std::string loc = trim_line(lines[base + 1]);
      nlohmann::json &frame = frames[exe_idx[k]];
      if (loc != "??:0") {
        const size_t colon = loc.find_last_of(':');
        if (colon != std::string::npos) {
          frame["filename"] = loc.substr(0, colon);
          try {
            frame["lineno"] = std::stoi(loc.substr(colon + 1));
          } catch (...) {
          }
        }
      }
      if (func != "??" && !func.empty()) {
        frame["function"] = func;
      }
    }
  }

  static void send_crash_to_sentry(int signum, const siginfo_t *info) {
    if (m_sentry_host.empty()) {
      return;
    }

    // Generate pseudo-unique event_id (32 hex chars)
    std::array<char, 33> event_id{};
    {
      std::random_device rd;
      uint32_t a = rd(), b = rd(), c = rd(), d = rd();
      snprintf(event_id.data(), event_id.size(), "%08x%08x%08x%08x", a, b, c, d);
    }

    // Build event JSON (truncated stack for safety; bounded by kMaxFrames)
    nlohmann::json event_json{{"event_id", event_id.data()},
                              {"level", "fatal"},
                              {"platform", "native"}};
    std::string fault_addr;
    if (info != nullptr) {
      char addr[32];
      snprintf(addr, sizeof(addr), "%p", info->si_addr);
      fault_addr = addr;
    }
    nlohmann::json exception_value{
        {"type", signal_name(signum)},
        {"value", fault_addr},
        {"stacktrace", {{"frames", nlohmann::json::array()}}}};
    const int fault_idx = find_faulting_frame_index();
    for (int i = 0; i < m_crash_frame_count; ++i) {
      char addr[32];
      snprintf(addr, sizeof(addr), "%lx",
               reinterpret_cast<unsigned long>(m_crash_frames[i]));
      nlohmann::json frame{{"instruction_addr", std::string("0x") + addr}};
      Dl_info dli{};
      if (dladdr(m_crash_frames[i], &dli) != 0 && dli.dli_sname != nullptr) {
        frame["function"] = demangle_symbol(dli.dli_sname);
        frame["filename"] =
            dli.dli_fname != nullptr ? std::string(dli.dli_fname) : "l2-proxy";
      }
      exception_value["stacktrace"]["frames"].push_back(std::move(frame));
    }
    annotate_exe_frames(exception_value["stacktrace"]["frames"]);

    std::string faulting_frame =
        m_crash_frame_count > 0 ? describe_frame(m_crash_frames[fault_idx])
                                : "?";
    nlohmann::json &frames = exception_value["stacktrace"]["frames"];
    if (fault_idx >= 0 && static_cast<size_t>(fault_idx) < frames.size() &&
        frames[fault_idx].contains("lineno")) {
      faulting_frame +=
          " " + frames[fault_idx]["filename"].get<std::string>() + ":" +
          std::to_string(frames[fault_idx]["lineno"].get<int>());
    }
    event_json["message"] =
        std::string(signal_name(signum)) + ": " + faulting_frame;
    exception_value["value"] = faulting_frame;
    event_json["exception"]["values"] =
        nlohmann::json::array({exception_value});
    event_json["tags"]["signal"] = signal_name(signum);

    const std::string event_str = event_json.dump();

    // Build envelope
    const nlohmann::json header = {
        {"event_id", event_id.data()},
        {"dsn", m_sentry_dsn_raw},
        {"sdk", {{"name", "http-data-diod"}, {"version", g_l2_proxy_version}}}};
    const nlohmann::json item_header = {
        {"type", "event"}, {"length", event_str.size()}};
    const std::string envelope =
        header.dump() + "\n" + item_header.dump() + "\n" + event_str;

    // Connect and send (best-effort; child process will exit regardless)
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
      return;
    }
    struct hostent *server = gethostbyname(m_sentry_host.c_str());
    if (server == nullptr) {
      close(sockfd);
      return;
    }
    struct sockaddr_in serv_addr {};
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(m_sentry_port);
    if (connect(sockfd,
                reinterpret_cast<struct sockaddr *>(&serv_addr),
                sizeof(serv_addr)) < 0) {
      close(sockfd);
      return;
    }

    // Build minimal HTTP request
    std::string http_req;
    http_req.reserve(envelope.size() + 512);
    http_req += "POST /api/";
    http_req += m_sentry_project;
    http_req += "/envelope/ HTTP/1.1\r\nHost: ";
    http_req += m_sentry_host;
    http_req += "\r\nContent-Type: application/json\r\nX-Sentry-Auth: Sentry "
                "sentry_version=7, sentry_key=";
    http_req += m_sentry_key;
    http_req += ", sentry_client=http-data-diod/";
    http_req += g_l2_proxy_version;
    http_req += "\r\nContent-Length: " + std::to_string(envelope.size());
    http_req += "\r\nConnection: close\r\n\r\n";
    http_req += envelope;

    write(sockfd, http_req.data(), http_req.size());

    // Wait for the server to read the request and reply before exiting, so
    // the event is actually ingested (otherwise the container restart may
    // drop an unread connection). Connection: close makes the server send its
    // response then close, so read returns 0 (EOF).
    char sbuf[512];
    while (read(sockfd, sbuf, sizeof(sbuf)) > 0) {
    }
    close(sockfd);
  }

  static void signal_handler(int signum, siginfo_t *info,
                             void * /*context*/) {
    m_crash_frame_count = backtrace(m_crash_frames, kMaxFrames);
    write_crash_report(signum, info);

    // Send to Sentry in a short-lived child process (fork is async-signal-safe)
    pid_t child = fork();
    if (child == 0) {
      send_crash_to_sentry(signum, info);
      _exit(0);
    }

    // Give the crash-report child time to finish (dladdr, addr2line, HTTP)
    // before the container restart kills the whole cgroup (the parent is
    // PID 1). waitpid/nanosleep are async-signal-safe; bounded ~4s worst case.
    struct timespec wait_ts {};
    wait_ts.tv_nsec = 50 * 1000 * 1000;
    for (int i = 0; i < 80; ++i) {
      int wstatus = 0;
      const pid_t r = waitpid(child, &wstatus, WNOHANG);
      if (r == child || r == -1) {
        break;
      }
      nanosleep(&wait_ts, nullptr);
    }

    // Re-raise with default handler to generate core dump
    signal(signum, SIG_DFL);
    raise(signum);
  }

public:
#if __has_include(<stacktrace>)
  // C++23 std::stacktrace + std::print: non-signal-safe, for on-demand
  // diagnostics (e.g. GET /debug/stacktrace). Dumps current thread trace via
  // std::stacktrace::current() + std::println — now links with -lstdc++exp on
  // GCC 15/16 (see CMakeLists.txt).
  static void log_current_stacktrace() {
    auto trace = std::stacktrace::current();
    std::println(stderr, "=== C++23 stacktrace ({} frames) ===", trace.size());
    for (std::size_t i = 0; i < trace.size(); ++i) {
      std::println(stderr, "#{} {} [{}:{}]", i, trace[i].description(),
                   trace[i].source_file(), trace[i].source_line());
    }
  }
#endif
};

#endif // CRASH_HANDLER_HPP

#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <format>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <spdlog/cfg/env.h>
#include <spdlog/formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

#include "config.hpp"

// Thread-local storage for correlation ID
struct LogContext {
  std::string m_request_id;
  std::string m_trace_id;
  std::string m_service_name;
  std::string m_client_ip;

  static LogContext &get() {
    thread_local LogContext ctx;
    return ctx;
  }

  static void clear() {
    auto &ctx = get();
    ctx.m_request_id.clear();
    ctx.m_trace_id.clear();
    ctx.m_client_ip.clear();
  }
};

// RAII scope that saves the current thread-local correlation context on
// construction and restores it on destruction. Put it at the entry point of
// every request handler so that all log lines for that request are correlated
// via request_id/trace_id/client_ip (and stale values are not leaked into the
// next request when httplib reuses the connection/thread).
class LogContextScope {
public:
  LogContextScope() {
    auto &ctx = LogContext::get();
    m_prev_request_id = ctx.m_request_id;
    m_prev_trace_id = ctx.m_trace_id;
    m_prev_client_ip = ctx.m_client_ip;
  }

  ~LogContextScope() {
    auto &ctx = LogContext::get();
    ctx.m_request_id = std::move(m_prev_request_id);
    ctx.m_trace_id = std::move(m_prev_trace_id);
    ctx.m_client_ip = std::move(m_prev_client_ip);
  }

  LogContextScope(const LogContextScope &) = delete;
  LogContextScope &operator=(const LogContextScope &) = delete;

private:
  std::string m_prev_request_id;
  std::string m_prev_trace_id;
  std::string m_prev_client_ip;
};

// Custom JSON formatter for structured logging
namespace spdlog {
namespace custom {
class JsonFormatter : public formatter {
public:
  JsonFormatter() : m_include_location(true) {}

  void format(const spdlog::details::log_msg &msg,
              memory_buf_t &dest) override {
    nlohmann::json j;

    // Timestamp in ISO 8601 format (UTC)
    const auto now_s =
        std::chrono::time_point_cast<std::chrono::seconds>(msg.time);

    // Add milliseconds
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            msg.time.time_since_epoch())
                            .count() %
                        1000;
    j["timestamp"] = std::format("{:%Y-%m-%dT%H:%M:%S}.{:03}Z", now_s, millis);

    // Level - convert string_view to string properly
    const auto level_sv = spdlog::level::to_string_view(msg.level);
    std::string level_str(level_sv.data(), level_sv.size());
    j["level"] = level_str;

    // Service name (from thread-local context)
    const auto &ctx = LogContext::get();
    if (!ctx.m_service_name.empty()) {
      j["service"] = ctx.m_service_name;
    } else {
      j["service"] =
          std::string(msg.logger_name.begin(), msg.logger_name.end());
    }

    // Message - convert from fmt::memory_buffer to string properly
    std::string msg_str(msg.payload.begin(), msg.payload.end());
    j["message"] = msg_str;

    // Correlation IDs (from thread-local context)
    if (!ctx.m_request_id.empty()) {
      j["request_id"] = ctx.m_request_id;
    }
    if (!ctx.m_trace_id.empty()) {
      j["trace_id"] = ctx.m_trace_id;
    }
    if (!ctx.m_client_ip.empty()) {
      j["client_ip"] = ctx.m_client_ip;
    }

    // Thread ID
    j["thread_id"] = msg.thread_id;

    // Source location (if available)
    if (m_include_location && !msg.source.empty()) {
      j["source"] = {{"file", std::string(msg.source.filename)},
                     {"line", msg.source.line},
                     {"func", std::string(msg.source.funcname)}};
    }

    // Add newline
    const auto json_str = j.dump(-1, ' ', false); // No indent for compact logs
    dest.append(json_str.data(), json_str.data() + json_str.size());
    dest.push_back('\n');
  }

  std::unique_ptr<formatter> clone() const override {
    return spdlog::details::make_unique<JsonFormatter>(*this);
  }

private:
  bool m_include_location;
};

// Text formatter used instead of the built-in pattern so that every line can
// carry the thread id and the thread-local correlation context
// (request_id/trace_id/client_ip). ANSI colors are applied only around the
// level, and only when use_colors is true (i.e. text mode on a real TTY).
class TextFormatter : public formatter {
public:
  explicit TextFormatter(bool use_colors) : m_use_colors(use_colors) {}

  void format(const spdlog::details::log_msg &msg,
              memory_buf_t &dest) override {
    // Timestamp in UTC with milliseconds (matches JsonFormatter)
    const auto now_s =
        std::chrono::time_point_cast<std::chrono::seconds>(msg.time);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            msg.time.time_since_epoch())
                            .count() %
                        1000;
    std::string ts = std::format("{:%Y-%m-%d %H:%M:%S}.{:03}", now_s, millis);
    append(dest, ts);

    // Level (optionally colored in a real terminal)
    const auto level_sv = spdlog::level::to_string_view(msg.level);
    std::string level_str(level_sv.data(), level_sv.size());
    if (m_use_colors) {
      append(dest, level_color(msg.level));
    }
    append(dest, "[");
    append(dest, level_str);
    append(dest, "]");
    if (m_use_colors) {
      append(dest, "\033[0m");
    }

    // Thread ID
    append(dest, " [thread=");
    append(dest, std::to_string(msg.thread_id));
    append(dest, "]");

    // Correlation context (populated by LogContextScope)
    const auto &ctx = LogContext::get();
    if (!ctx.m_request_id.empty() || !ctx.m_trace_id.empty() ||
        !ctx.m_client_ip.empty()) {
      append(dest, " [");
      bool first = true;
      if (!ctx.m_request_id.empty()) {
        append(dest, "request_id=");
        append(dest, ctx.m_request_id);
        first = false;
      }
      if (!ctx.m_trace_id.empty()) {
        if (!first) {
          append(dest, " ");
        }
        append(dest, "trace_id=");
        append(dest, ctx.m_trace_id);
        first = false;
      }
      if (!ctx.m_client_ip.empty()) {
        if (!first) {
          append(dest, " ");
        }
        append(dest, "client_ip=");
        append(dest, ctx.m_client_ip);
      }
      append(dest, "]");
    }

    append(dest, " ");
    append(dest, std::string_view(msg.payload.data(), msg.payload.size()));
    dest.push_back('\n');
  }

  std::unique_ptr<formatter> clone() const override {
    return spdlog::details::make_unique<TextFormatter>(m_use_colors);
  }

private:
  bool m_use_colors;

  static void append(memory_buf_t &dest, std::string_view sv) {
    dest.append(sv.data(), sv.data() + sv.size());
  }

  static std::string level_color(spdlog::level::level_enum level) {
    // error   -> red
    // warning -> pink/magenta
    // info    -> white
    // debug   -> gray
    switch (level) {
    case spdlog::level::debug:
      return "\033[90m";
    case spdlog::level::info:
      return "\033[97m";
    case spdlog::level::warn:
      return "\033[95m";
    default:
      return "\033[91m";
    }
  }
};
} // namespace custom
} // namespace spdlog

// spdlog-based Logger wrapper for backward compatibility
// Usage: Logger::info("message") or spdlog::info("message")
class Logger {
public:
  enum Level : uint8_t { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

  // Initialize logger (call once at startup)
  static void init() {
    std::call_once(s_init_flag, []() {
      const std::string logger_name = default_logger_name();

      // Check if logger already exists globally
      const auto existing_logger = spdlog::get(logger_name);
      if (existing_logger) {
        s_logger = existing_logger;
        spdlog::set_default_logger(s_logger);
        s_initialized = true;
        return;
      }

      // Check log format (text or json). Uses the silent helper: Logger::init
      // runs inside std::call_once, so it must not trigger any logging.
      const std::string log_format =
          Config::get_env_string_silent("LOG_FORMAT", "text");
      bool use_json_format = log_format == "json" || log_format == "JSON";

      // ANSI colors are only enabled on a real terminal (TTY). Docker logs and
      // piped output get plain text so they don't contain escape sequences.
      const bool use_colors = ::isatty(STDOUT_FILENO) && !use_json_format;

      // Plain stdout sink: coloring is handled by TextFormatter above (text
      // mode on a TTY only), so docker/piped output stays machine-readable.
      auto console_sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
      console_sink->set_level(spdlog::level::debug);

      // Create rotating file sink (10MB max, 3 files)
      auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          "logs/l2-proxy.log", 10 * 1024 * 1024, 3);
      file_sink->set_level(spdlog::level::debug);

      // Create multi-sink logger
      s_logger = std::make_shared<spdlog::logger>(
          logger_name, spdlog::sinks_init_list{console_sink, file_sink});
      s_logger->set_level(spdlog::level::debug);
      s_logger->flush_on(spdlog::level::info);

      // Set formatter based on LOG_FORMAT environment variable. Formatters are
      // set per-sink (after the logger constructor overwrites them with the
      // default pattern formatter). The file sink never emits colors.
      if (use_json_format) {
        console_sink->set_formatter(
            std::make_unique<spdlog::custom::JsonFormatter>());
        file_sink->set_formatter(
            std::make_unique<spdlog::custom::JsonFormatter>());
      } else {
        console_sink->set_formatter(
            std::make_unique<spdlog::custom::TextFormatter>(use_colors));
        file_sink->set_formatter(
            std::make_unique<spdlog::custom::TextFormatter>(false));
      }

      spdlog::register_logger(s_logger);
      spdlog::set_default_logger(s_logger);

      s_colors_enabled.store(use_colors);

      if (use_json_format) {
        spdlog::info("Using structured JSON log format");
      } else {
        spdlog::info("Using plain text log format{}",
                     use_colors ? " with colors" : "");
      }

      // Check LOG_LEVEL environment variable (from docker-compose)
      const char *log_level_env = std::getenv("LOG_LEVEL");
      if (log_level_env) {
        std::string log_level = std::string(log_level_env);
        // Convert to uppercase for comparison
        std::transform(log_level.begin(), log_level.end(), log_level.begin(),
                       ::toupper);

        spdlog::level::level_enum spdlog_level = spdlog::level::info; // default

        if (log_level == "DEBUG") {
          spdlog_level = spdlog::level::debug;
        } else if (log_level == "WARN" || log_level == "WARNING") {
          spdlog_level = spdlog::level::warn;
        } else if (log_level == "ERROR") {
          spdlog_level = spdlog::level::err;
        } else if (log_level == "CRITICAL") {
          spdlog_level = spdlog::level::critical;
        } else if (log_level == "OFF") {
          spdlog_level = spdlog::level::off;
        }

        // Set level for logger and all sinks
        s_logger->set_level(spdlog_level);
        console_sink->set_level(spdlog_level);
        file_sink->set_level(spdlog_level);
        spdlog::set_level(spdlog_level);

        s_logger->info(
            "Log level set to {} (from LOG_LEVEL environment variable)",
            log_level);
      } else {
        // Fallback to SPDLOG_LEVEL if LOG_LEVEL not set
        spdlog::cfg::load_env_levels();
      }

      s_initialized = true;
    });
  }

  // Set log level
  static void set_level(Level level) {
    init();
    // Logger::Level and spdlog::level::level_enum use different numeric
    // values, so map explicitly instead of casting (a cast would silently
    // select a more verbose level, e.g. INFO -> spdlog::debug).
    spdlog::level::level_enum spdlog_level = spdlog::level::off;
    switch (level) {
    case DEBUG:
      spdlog_level = spdlog::level::debug;
      break;
    case INFO:
      spdlog_level = spdlog::level::info;
      break;
    case WARN:
      spdlog_level = spdlog::level::warn;
      break;
    case ERROR:
      spdlog_level = spdlog::level::err;
      break;
    }
    s_logger->set_level(spdlog_level);
    spdlog::set_level(spdlog_level);
  }

  static void set_level_from_string(const std::string &level_str) {
    init();
    if (level_str == "DEBUG" || level_str == "debug") {
      set_level(DEBUG);
    } else if (level_str == "INFO" || level_str == "info") {
      set_level(INFO);
    } else if (level_str == "WARN" || level_str == "warn" ||
               level_str == "WARNING" || level_str == "warning") {
      set_level(WARN);
    } else if (level_str == "ERROR" || level_str == "error") {
      set_level(ERROR);
    } else {
      spdlog::warn("Unknown log level: {}, defaulting to INFO", level_str);
      set_level(INFO);
    }
  }

  // Correlation context management
  static void set_request_id(const std::string &request_id) {
    LogContext::get().m_request_id = request_id;
  }

  static void set_trace_id(const std::string &trace_id) {
    LogContext::get().m_trace_id = trace_id;
  }

  static void set_client_ip(const std::string &client_ip) {
    LogContext::get().m_client_ip = client_ip;
  }

  static void set_service_name(const std::string &service_name) {
    LogContext::get().m_service_name = service_name;
  }

  static void clear_correlation_context() { LogContext::clear(); }

  // Whether ANSI colors are enabled for console output (text mode on a TTY)
  static bool is_colored_output() { return s_colors_enabled.load(); }

  // Backward compatible logging methods
  static void debug(const std::string &msg) {
    init();
    spdlog::debug(msg);
  }
  static void info(const std::string &msg) {
    init();
    spdlog::info(msg);
  }
  static void warn(const std::string &msg) {
    init();
    spdlog::warn(msg);
  }
  static void error(const std::string &msg) {
    init();
    spdlog::error(msg);
  }

  // Formatted logging (spdlog style)
  template <typename... Args>
  static void debug(fmt::format_string<Args...> fmt, Args &&...args) {
    init();
    spdlog::debug(fmt, std::forward<Args>(args)...);
  }
  template <typename... Args>
  static void info(fmt::format_string<Args...> fmt, Args &&...args) {
    init();
    spdlog::info(fmt, std::forward<Args>(args)...);
  }
  template <typename... Args>
  static void warn(fmt::format_string<Args...> fmt, Args &&...args) {
    init();
    spdlog::warn(fmt, std::forward<Args>(args)...);
  }
  template <typename... Args>
  static void error(fmt::format_string<Args...> fmt, Args &&...args) {
    init();
    spdlog::error(fmt, std::forward<Args>(args)...);
  }

  // Get current log level
  static Level get_level() {
    init();
    switch (s_logger->level()) {
    case spdlog::level::trace:
    case spdlog::level::debug:
      return DEBUG;
    case spdlog::level::info:
      return INFO;
    case spdlog::level::warn:
      return WARN;
    default:
      return ERROR;
    }
  }

private:
  static std::shared_ptr<spdlog::logger> s_logger;
  static bool s_initialized;
  static std::atomic<bool> s_colors_enabled;
  // Single initialization guard for Logger::init(). Inline so the header
  // defines exactly one object program-wide (function-local statics inside an
  // implicitly-inline member function are a shared object, but an inline data
  // member is explicit and analyzer-friendly).
  static inline std::once_flag s_init_flag;

  // Logger name doubles as the default "service" field (JsonFormatter falls
  // back to msg.logger_name when the thread-local context is empty), so it is
  // derived from MODE to distinguish the three services in aggregated logs.
  static std::string default_logger_name() {
    const std::string mode = Config::get_env_string_silent("MODE", "proxy");
    if (mode == "worker") {
      return "l2-worker";
    }
    if (mode == "l2-server") {
      return "l2-server";
    }
    return "l2-proxy";
  }
};

// Initialize static members
inline std::shared_ptr<spdlog::logger> Logger::s_logger = nullptr;
inline bool Logger::s_initialized = false;
inline std::atomic<bool> Logger::s_colors_enabled{false};

#endif // LOGGER_HPP

#ifndef TIME_UTILS_HPP
#define TIME_UTILS_HPP

#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <string>

class TimeUtils {
public:
  static int64_t epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  static int64_t epoch_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  static int64_t epoch_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  // Monotonic milliseconds since an unspecified origin. Suitable for TTL and
  // eviction logic that only needs elapsed-time deltas (steady_clock never
  // jumps). Shared by the dedup cache, duplicate detector and metric caches.
  static uint64_t steady_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

  // Monotonic microseconds (deltas only). Used by circuit-breaker timeouts so
  // an NTP wall-clock jump cannot prematurely trip the breaker.
  static uint64_t steady_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

  static std::string format_rfc3339() {
    return format_rfc3339_us(static_cast<uint64_t>(epoch_us()));
  }

  // RFC3339 with millisecond precision from an absolute epoch-microsecond
  // value. Used by the tracing sender for span start/end timestamps, which
  // carry wall-clock time (Sentry transactions require absolute ISO timestamps
  // while the span queue keeps durations in monotonic microseconds).
  static std::string format_rfc3339_us(uint64_t epoch_us) {
    const auto tp = std::chrono::system_clock::time_point(
        std::chrono::microseconds(static_cast<int64_t>(epoch_us)));
    const auto tp_s =
        std::chrono::time_point_cast<std::chrono::seconds>(tp);
    const auto ms = (epoch_us / 1000) % 1000;
    return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03}Z", tp_s, ms);
  }

  static int64_t
  ms_until(const std::chrono::system_clock::time_point &deadline) {
    const auto now = std::chrono::system_clock::now();
    if (deadline <= now)
      return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
        .count();
  }

  // Converts a microsecond interval into seconds. Shared by the latency
  // bookkeeping that previously repeated the /1000000.0 division by hand.
  static double duration_seconds(uint64_t start_us, uint64_t end_us) {
    return static_cast<double>(end_us - start_us) / 1000000.0;
  }
};

#endif
#ifndef TIME_UTILS_HPP
#define TIME_UTILS_HPP

#include <chrono>
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

  static std::string format_rfc3339() {
    const auto now = std::chrono::system_clock::now();
    const auto now_s = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch())
                        .count() %
                    1000;
    return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03}Z", now_s, ms);
  }

  static std::string format_iso8601() { return format_rfc3339(); }

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
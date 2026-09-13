#include "request_id_generator.hpp"
#include "random_utils.hpp"
#include "time_utils.hpp"
#include <chrono>
#include <ctime>
#include <format>

const int RequestIdGenerator::g_default_random_digits;

thread_local std::string RequestIdGenerator::cached_date_str;

// Request ID format: YYYY-MM-DD~<counter>~<6-digit-zero-padded-random>.
// The date part is cached per-thread for up to an hour to avoid a system call
// on every request while keeping the counter/random suffixes unique.
std::string RequestIdGenerator::generate_uuid() {
  static thread_local auto last_date_update =
      std::chrono::steady_clock::time_point();

  const auto steady_now = std::chrono::steady_clock::now();
  if (cached_date_str.empty() ||
      steady_now - last_date_update > std::chrono::hours(1)) {
    const auto time_t_now = static_cast<time_t>(TimeUtils::epoch_s());
    std::tm tm_now;
    // Use thread-safe localtime_r instead of localtime
    localtime_r(&time_t_now, &tm_now);
    cached_date_str = std::format("{:04d}-{:02d}-{:02d}",
                                  tm_now.tm_year + 1900, tm_now.tm_mon + 1,
                                  tm_now.tm_mday);
    last_date_update = steady_now;
  }

  return std::format("{}~{}~{:06d}", cached_date_str, m_counter++,
                     RandomUtils::between(0, 999999));
}

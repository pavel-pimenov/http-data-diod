#include "request_id_generator.hpp"
#include "random_utils.hpp"
#include "time_utils.hpp"
#include <chrono>
#include <iomanip>

const int RequestIdGenerator::g_default_random_digits;

thread_local std::string RequestIdGenerator::cached_date_str;
thread_local std::stringstream RequestIdGenerator::date_ss;
thread_local std::stringstream RequestIdGenerator::result_ss;

std::string RequestIdGenerator::generate_uuid() {
  date_ss.str("");
  date_ss.clear();
  result_ss.str("");
  result_ss.clear();

  // Get current date in YYYY-MM-DD format (cached per-thread for performance)
  static thread_local auto last_date_update =
      std::chrono::steady_clock::time_point();

  // Update date string only once per hour to reduce system calls.
  // cached_date_str is a thread_local member, so no cross-thread data race.
  const auto steady_now = std::chrono::steady_clock::now();
  if (steady_now - last_date_update > std::chrono::hours(1) ||
      cached_date_str.empty()) {
    const auto epoch_s = TimeUtils::epoch_s();
    const auto time_t_now = static_cast<time_t>(epoch_s);
    std::tm tm_now;
    // Use thread-safe localtime_r instead of localtime
    localtime_r(&time_t_now, &tm_now);
    date_ss << std::put_time(&tm_now, "%Y-%m-%d");
    cached_date_str = date_ss.str();
    last_date_update = steady_now;
  }

  // Increment counter for unique UUID
  const long long counter = m_counter++;

  // Generate 6-digit random number using thread-local storage
  const int random_num = RandomUtils::between(0, 999999);

  // Concatenate: date + counter + random
  result_ss << cached_date_str << '~' << counter << '~' << std::setfill('0')
            << std::setw(6) << random_num;
  return result_ss.str();
}
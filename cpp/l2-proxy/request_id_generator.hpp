#ifndef REQUEST_ID_GENERATOR_HPP
#define REQUEST_ID_GENERATOR_HPP

#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>

class RequestIdGenerator {
private:
  static const int g_default_random_digits = 6;
  std::atomic<long long> m_counter{0};

  // Thread-local storage for date caching
  static thread_local std::string cached_date_str;
  static thread_local std::stringstream date_ss;
  static thread_local std::stringstream result_ss;

public:
  RequestIdGenerator() = default;
  ~RequestIdGenerator() = default;

  std::string generate_uuid();
};

#endif // REQUEST_ID_GENERATOR_HPP
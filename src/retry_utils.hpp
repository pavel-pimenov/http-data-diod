#ifndef RETRY_UTILS_HPP
#define RETRY_UTILS_HPP

#include "logger.hpp"
#include "random_utils.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <string>
#include <thread>

namespace retry_defaults {
inline constexpr int kBaseDelayMs = 100;
inline constexpr int kJitterPercent = 50;
inline constexpr int kMaxRetries = 3;
inline constexpr int kMaxDelayMs = 2000;
inline constexpr int kJitterMs = 50;
} // namespace retry_defaults

// ============================================================================
// Exponential Backoff with Jitter
// ============================================================================

// Exponential backoff: base * 2^(attempt-1), capped at max_delay_ms, plus a
// uniform jitter in [0, jitter_ms].
inline int calculate_retry_delay_with_jitter(int base_delay_ms, int jitter_ms,
                                             int attempt, int max_delay_ms) {
  const int exponential_delay = base_delay_ms * (1 << (attempt - 1));
  const int capped_delay = std::min(exponential_delay, max_delay_ms);
  return capped_delay + RandomUtils::between(0, jitter_ms);
}

// Base delay plus a jitter of up to jitter_percent% of the base delay.
inline int calculate_jitter_delay(int base_delay_ms, int jitter_percent = retry_defaults::kJitterPercent) {
  if (base_delay_ms <= 0) {
    return 0;
  }
  const int max_jitter = (base_delay_ms * jitter_percent) / 100;
  if (max_jitter <= 0) {
    return base_delay_ms;
  }
  return base_delay_ms + RandomUtils::between(0, max_jitter - 1);
}

// Base delay plus an absolute jitter in [0, jitter_ms].
// Computes a per-attempt delay (base*attempt plus jitter) and sleeps the
// calling thread before the next retry attempt. Shared by retry loops that
// otherwise duplicated the identical delay+sleep pair.
inline void sleep_for_attempt_jitter(
    int attempt, int base_delay_ms = retry_defaults::kBaseDelayMs,
    int jitter_percent = retry_defaults::kJitterPercent) {
  const int delay_ms =
      calculate_jitter_delay(base_delay_ms * attempt, jitter_percent);
  std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}// ============================================================================
// Retry Loop Helper
// ============================================================================
// NB: Result is the return type, Operation is a callable object.
template <typename Result, typename Operation>
inline Result execute_with_retry(
    Operation &&operation, const std::string &operation_name,
    int max_retries = retry_defaults::kMaxRetries,
    int base_delay_ms = retry_defaults::kBaseDelayMs,
    int max_delay_ms = retry_defaults::kMaxDelayMs,
    int jitter_ms = retry_defaults::kJitterMs) {
  std::string last_error;

  for (int attempt = 1; attempt <= max_retries; ++attempt) {
    try {
      return operation();
    } catch (const std::exception &e) {
      last_error = e.what();

      if (attempt < max_retries) {
        int delay = calculate_retry_delay_with_jitter(base_delay_ms, jitter_ms,
                                                      attempt, max_delay_ms);
        Logger::warn("{} attempt {} failed: {}. Retrying in {}ms...",
                     operation_name, attempt, last_error, delay);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
      }
    }
  }

  throw std::runtime_error(
      std::format("{} failed after {} retries. Last error: {}", operation_name,
                  max_retries, last_error));
}


#endif // RETRY_UTILS_HPP


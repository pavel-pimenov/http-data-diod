#ifndef RATE_LIMITER_HPP
#define RATE_LIMITER_HPP

#include "logger.hpp"
#include <atomic>
#include <chrono>
#include <mutex>

// Token Bucket Rate Limiter
// Controls request rate to prevent overload
//
// Usage:
//   RateLimiter limiter(1000, 100);  // 1000 tokens max, 100 tokens/sec refill
//   if (limiter.acquire()) {
//       // Process request
//   } else {
//       // Return 429 Too Many Requests
//   }

class RateLimiter {
private:
  const uint64_t m_max_tokens;
  const uint64_t m_refill_tokens_per_second;

  std::atomic<uint64_t> m_tokens;
  std::chrono::steady_clock::time_point m_last_refill;
  std::mutex m_mutex;

  std::atomic<uint64_t> m_total_requests{0};
  std::atomic<uint64_t> m_allowed_requests{0};
  std::atomic<uint64_t> m_rejected_requests{0};

public:
  RateLimiter(uint64_t max_tokens, uint64_t refill_tokens_per_second)
      : m_max_tokens(max_tokens),
        m_refill_tokens_per_second(refill_tokens_per_second),
        m_tokens(max_tokens), m_last_refill(std::chrono::steady_clock::now()) {
    Logger::info("RateLimiter initialized: max={} tokens, refill={}/sec",
                 max_tokens, refill_tokens_per_second);
  }

  // Returns true if request is allowed, false if rate limited
  bool acquire() {
    m_total_requests.fetch_add(1, std::memory_order_relaxed);

    refill();

    uint64_t expected = m_tokens.load(std::memory_order_acquire);
    while (expected > 0) {
      if (m_tokens.compare_exchange_weak(expected, expected - 1,
                                         std::memory_order_acq_rel)) {
        m_allowed_requests.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }

    m_rejected_requests.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  uint64_t available_tokens() const {
    return m_tokens.load(std::memory_order_acquire);
  }

  uint64_t max_tokens() const { return m_max_tokens; }

  uint64_t refill_rate() const { return m_refill_tokens_per_second; }

private:
  void refill() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - m_last_refill)
                             .count();

    if (elapsed >= 1000) {
      uint64_t tokens_to_add = (elapsed / 1000) * m_refill_tokens_per_second;
      uint64_t new_tokens =
          m_tokens.load(std::memory_order_relaxed) + tokens_to_add;

      if (new_tokens > m_max_tokens) {
        new_tokens = m_max_tokens;
      }

      m_tokens.store(new_tokens, std::memory_order_release);
      m_last_refill = now;

      Logger::debug("RateLimiter refilled: +{} tokens, total={}", tokens_to_add,
                    new_tokens);
    }
  }
};

#endif // RATE_LIMITER_HPP

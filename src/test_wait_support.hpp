#ifndef TEST_WAIT_SUPPORT_HPP
#define TEST_WAIT_SUPPORT_HPP

#include <chrono>
#include <thread>

// Test-only "poll until the condition holds" helper, shared by every test TU
// that waits for asynchronous state (metric counters, background threads,
// loopback servers).
//
// It has its own header instead of living in test_support.hpp because that one
// pulls app_context/request_handler/stats_logger, and the dependency-light test
// TUs (trace logger, coverage extras) only need the wait loop. `test_`-prefixed
// file name, so it stays out of the per-file coverage gate.

namespace test_wait_support {

// Polls `predicate` every `poll_interval` until it returns true or `timeout_ms`
// elapses. The predicate is also evaluated once after the deadline, so a
// condition that became true just as the timeout expired still counts and an
// already-true condition costs one call and no sleep. `poll_interval` is a
// parameter because tests that watch an atomic/queue need 1 ms granularity
// while the rest are fine with the 50 ms default.
template <typename Predicate>
[[nodiscard]] inline bool wait_for_condition(
    const Predicate &predicate, int timeout_ms,
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds(50)) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(poll_interval);
  }
  return predicate();
}

} // namespace test_wait_support

#endif

#ifndef RANDOM_UTILS_HPP
#define RANDOM_UTILS_HPP

#include <random>

namespace RandomUtils {

// Shared per-thread RNG — single source for all jitter/random-number sites.
inline std::mt19937_64 &rng() {
  thread_local std::mt19937_64 gen{std::random_device{}()};
  return gen;
}

// Random integer in the inclusive range [lo, hi]
inline int between(int lo, int hi) {
  std::uniform_int_distribution<int> dist(lo, hi);
  return dist(rng());
}

} // namespace RandomUtils

#endif // RANDOM_UTILS_HPP

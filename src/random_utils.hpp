#ifndef RANDOM_UTILS_HPP
#define RANDOM_UTILS_HPP

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

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

// Lowercase hex string of `len` characters — one implementation for every
// random identifier in the project (trace/span ids, Sentry event ids). Chars
// are drawn 16 at a time from the shared per-thread RNG.
inline std::string hex_string(std::size_t len) {
  static constexpr std::string_view k_hex_digits = "0123456789abcdef";
  std::string out;
  out.reserve(len);
  for (std::size_t i = 0; i < len; i += 16) {
    const uint64_t value = rng()();
    for (std::size_t j = 0; j < 16 && i + j < len; ++j) {
      out.push_back(k_hex_digits[(value >> (4 * j)) & 0xF]);
    }
  }
  return out;
}

} // namespace RandomUtils

#endif // RANDOM_UTILS_HPP

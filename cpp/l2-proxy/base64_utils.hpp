#ifndef BASE64_UTILS_HPP
#define BASE64_UTILS_HPP

#include "base64/base64.hpp"
#include <string>

namespace base64 {
// Thin compatibility layer over the canonical implementation in
// base64/base64.hpp. Keeps the legacy encode()/decode() names for existing
// call sites; new code should use to_base64()/from_base64() directly.

inline std::string encode(const std::string &input) { return to_base64(input); }

// Decodes base64 back to the original bytes.
// Throws std::runtime_error on malformed input.
inline std::string decode(const std::string &input) {
  return from_base64(input);
}
} // namespace base64

#endif // BASE64_UTILS_HPP

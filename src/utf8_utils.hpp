#ifndef UTF8_UTILS_HPP
#define UTF8_UTILS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace utf8_utils {

// Length of the valid UTF-8 sequence starting at index, or 0 when the bytes at
// index do not form a valid sequence. Rejects overlong forms, UTF-16 surrogates
// and code points above U+10FFFF, so the result matches what nlohmann::json
// accepts when serializing a string.
inline std::size_t sequence_length(std::string_view input, std::size_t index) {
  constexpr std::array<std::uint32_t, 4> min_codepoint = {0, 0x80, 0x800,
                                                          0x10000};
  constexpr std::uint32_t max_codepoint = 0x10FFFF;
  constexpr std::uint32_t surrogate_first = 0xD800;
  constexpr std::uint32_t surrogate_last = 0xDFFF;

  const auto *bytes = reinterpret_cast<const unsigned char *>(input.data());
  const std::size_t size = input.size();
  const unsigned char first = bytes[index];
  if (first < 0x80) {
    return 1;
  }

  std::size_t extra = 0;
  std::uint32_t codepoint = 0;
  if ((first & 0xE0) == 0xC0) {
    extra = 1;
    codepoint = first & 0x1F;
  } else if ((first & 0xF0) == 0xE0) {
    extra = 2;
    codepoint = first & 0x0F;
  } else if ((first & 0xF8) == 0xF0) {
    extra = 3;
    codepoint = first & 0x07;
  } else {
    return 0;
  }

  if (index + extra >= size) {
    return 0;
  }
  for (std::size_t offset = 1; offset <= extra; ++offset) {
    const unsigned char next = bytes[index + offset];
    if ((next & 0xC0) != 0x80) {
      return 0;
    }
    codepoint = (codepoint << 6) | (next & 0x3F);
  }

  if (codepoint < min_codepoint[extra] || codepoint > max_codepoint ||
      (codepoint >= surrogate_first && codepoint <= surrogate_last)) {
    return 0;
  }
  return extra + 1;
}

inline bool is_valid_utf8(std::string_view input) {
  std::size_t index = 0;
  while (index < input.size()) {
    const std::size_t length = sequence_length(input, index);
    if (length == 0) {
      return false;
    }
    index += length;
  }
  return true;
}

// Replaces every invalid byte with U+FFFD so the result can be serialized by
// nlohmann::json. Legacy upstreams put localized text into HTTP headers (PTOKAX
// sends a Windows-1251 Date), and a single such byte used to abort the whole
// NATS response with json type_error.316.
inline std::string sanitize_invalid(std::string_view input) {
  constexpr std::string_view replacement = "\xEF\xBF\xBD";
  std::string result;
  result.reserve(input.size());
  std::size_t index = 0;
  while (index < input.size()) {
    const std::size_t length = sequence_length(input, index);
    if (length == 0) {
      result.append(replacement);
      ++index;
    } else {
      result.append(input.substr(index, length));
      index += length;
    }
  }
  return result;
}

} // namespace utf8_utils

#endif // UTF8_UTILS_HPP

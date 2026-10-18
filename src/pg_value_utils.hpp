#ifndef PG_VALUE_UTILS_HPP
#define PG_VALUE_UTILS_HPP

#include "json_utils.hpp"
#include "logger.hpp"
#include <base64.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <string>

// Pure Postgres type-OID helpers of the DB gateway. Header-only and free of
// libpq so the unit tests can exercise the text-format value conversion
// (including its defensive branches) without a live server.

namespace pg_value_utils {

// PostgreSQL built-in type OIDs used for JSON value conversion and column
// metadata. Taken from pg_type.h of the stock server.
inline constexpr uint32_t kPgBoolOid = 16;
inline constexpr uint32_t kPgByteaOid = 17;
inline constexpr uint32_t kPgCharOid = 18;
inline constexpr uint32_t kPgNameOid = 19;
inline constexpr uint32_t kPgInt8Oid = 20;
inline constexpr uint32_t kPgInt2Oid = 21;
inline constexpr uint32_t kPgInt4Oid = 23;
inline constexpr uint32_t kPgTextOid = 25;
inline constexpr uint32_t kPgOidOid = 26;
inline constexpr uint32_t kPgFloat4Oid = 700;
inline constexpr uint32_t kPgFloat8Oid = 701;
inline constexpr uint32_t kPgBpcharOid = 1042;
inline constexpr uint32_t kPgVarcharOid = 1043;
inline constexpr uint32_t kPgDateOid = 1082;
inline constexpr uint32_t kPgTimeOid = 1083;
inline constexpr uint32_t kPgTimestampOid = 1114;
inline constexpr uint32_t kPgTimestamptzOid = 1184;
inline constexpr uint32_t kPgTimetzOid = 1266;
inline constexpr uint32_t kPgNumericOid = 1700;
inline constexpr uint32_t kPgJsonOid = 114;
inline constexpr uint32_t kPgJsonbOid = 3802;
inline constexpr uint32_t kPgUuidOid = 2950;

inline std::string pg_type_name(uint32_t oid) {
  switch (oid) {
  case kPgBoolOid:
    return "BOOLEAN";
  case kPgByteaOid:
    return "BYTEA";
  case kPgCharOid:
    return "CHAR";
  case kPgNameOid:
    return "NAME";
  case kPgInt2Oid:
    return "SMALLINT";
  case kPgInt4Oid:
    return "INTEGER";
  case kPgInt8Oid:
    return "BIGINT";
  case kPgTextOid:
    return "TEXT";
  case kPgOidOid:
    return "OID";
  case kPgFloat4Oid:
    return "REAL";
  case kPgFloat8Oid:
    return "DOUBLE PRECISION";
  case kPgBpcharOid:
    return "BPCHAR";
  case kPgVarcharOid:
    return "VARCHAR";
  case kPgDateOid:
    return "DATE";
  case kPgTimeOid:
    return "TIME";
  case kPgTimestampOid:
    return "TIMESTAMP";
  case kPgTimestamptzOid:
    return "TIMESTAMPTZ";
  case kPgTimetzOid:
    return "TIMETZ";
  case kPgNumericOid:
    return "NUMERIC";
  case kPgJsonOid:
    return "JSON";
  case kPgJsonbOid:
    return "JSONB";
  case kPgUuidOid:
    return "UUID";
  default:
    return std::format("UNKNOWN({})", oid);
  }
}

// Decodes PostgreSQL hex bytea output ("\x....") into raw bytes.
inline std::string hex_to_bytes(const std::string &hex) {
  const size_t start = (!hex.empty() && hex[0] == '\\' && hex.size() > 1 &&
                        hex[1] == 'x')
                           ? 2
                           : 0;
  const auto nibble = [](char c) -> unsigned char {
    if (c >= '0' && c <= '9') {
      return static_cast<unsigned char>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
      return static_cast<unsigned char>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
      return static_cast<unsigned char>(c - 'A' + 10);
    }
    return 0;
  };
  std::string out;
  out.reserve((hex.size() - start) / 2);
  for (size_t i = start; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  }
  return out;
}

// Converts a text-format value to the JSON value matching the column type.
inline json pg_value(uint32_t oid, const char *text, int len) {
  std::string s(text, len);
  switch (oid) {
  case kPgBoolOid:
    return len > 0 && text[0] == 't';
  case kPgInt2Oid:
  case kPgInt4Oid:
  case kPgInt8Oid:
  case kPgOidOid:
    try {
      return std::stoll(s);
    } catch (const std::exception &e) {
      Logger::debug("Non-numeric int8/oid value '{}', keeping raw string: {}",
                    s, e.what());
      return s;
    }
  case kPgFloat4Oid:
  case kPgFloat8Oid:
  case kPgNumericOid:
    try {
      return std::stod(s);
    } catch (const std::exception &e) {
      Logger::debug("Non-numeric float/numeric value '{}', keeping raw "
                    "string: {}",
                    s, e.what());
      return s;
    }
  case kPgByteaOid:
    return base64::to_base64(hex_to_bytes(s));
  default:
    return s;
  }
}

} // namespace pg_value_utils

#endif

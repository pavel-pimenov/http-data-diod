#include "pg_value_utils.hpp"

#include "json_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>

// Keep the import list explicit: no `using namespace` (google-build). The OID
// constants stay qualified at the call sites.
using pg_value_utils::hex_to_bytes;
using pg_value_utils::pg_type_name;
using pg_value_utils::pg_value;

TEST_CASE("pg_type_name maps every known OID and the unknown fallback") {
  REQUIRE(pg_type_name(pg_value_utils::kPgBoolOid) == "BOOLEAN");
  REQUIRE(pg_type_name(pg_value_utils::kPgByteaOid) == "BYTEA");
  REQUIRE(pg_type_name(pg_value_utils::kPgCharOid) == "CHAR");
  REQUIRE(pg_type_name(pg_value_utils::kPgNameOid) == "NAME");
  REQUIRE(pg_type_name(pg_value_utils::kPgInt2Oid) == "SMALLINT");
  REQUIRE(pg_type_name(pg_value_utils::kPgInt4Oid) == "INTEGER");
  REQUIRE(pg_type_name(pg_value_utils::kPgInt8Oid) == "BIGINT");
  REQUIRE(pg_type_name(pg_value_utils::kPgTextOid) == "TEXT");
  REQUIRE(pg_type_name(pg_value_utils::kPgOidOid) == "OID");
  REQUIRE(pg_type_name(pg_value_utils::kPgFloat4Oid) == "REAL");
  REQUIRE(pg_type_name(pg_value_utils::kPgFloat8Oid) == "DOUBLE PRECISION");
  REQUIRE(pg_type_name(pg_value_utils::kPgBpcharOid) == "BPCHAR");
  REQUIRE(pg_type_name(pg_value_utils::kPgVarcharOid) == "VARCHAR");
  REQUIRE(pg_type_name(pg_value_utils::kPgDateOid) == "DATE");
  REQUIRE(pg_type_name(pg_value_utils::kPgTimeOid) == "TIME");
  REQUIRE(pg_type_name(pg_value_utils::kPgTimestampOid) == "TIMESTAMP");
  REQUIRE(pg_type_name(pg_value_utils::kPgTimestamptzOid) == "TIMESTAMPTZ");
  REQUIRE(pg_type_name(pg_value_utils::kPgTimetzOid) == "TIMETZ");
  REQUIRE(pg_type_name(pg_value_utils::kPgNumericOid) == "NUMERIC");
  REQUIRE(pg_type_name(pg_value_utils::kPgJsonOid) == "JSON");
  REQUIRE(pg_type_name(pg_value_utils::kPgJsonbOid) == "JSONB");
  REQUIRE(pg_type_name(pg_value_utils::kPgUuidOid) == "UUID");
  REQUIRE(pg_type_name(999999) == "UNKNOWN(999999)");
}

TEST_CASE("hex_to_bytes decodes bytea hex output") {
  REQUIRE(hex_to_bytes("\\x48656c6c6f") == "Hello");
  REQUIRE(hex_to_bytes("\\x48656C6C6F") == "Hello");
  REQUIRE(hex_to_bytes("48656c6c6f") == "Hello");
  REQUIRE(hex_to_bytes("\\x4").empty());
  REQUIRE(hex_to_bytes("\\x").empty());
  REQUIRE(hex_to_bytes("").empty());
  REQUIRE(hex_to_bytes("\\xzz") == std::string("\x00", 1));
}

TEST_CASE("pg_value converts scalar text values") {
  REQUIRE(pg_value(pg_value_utils::kPgBoolOid, "t", 1) == json(true));
  REQUIRE(pg_value(pg_value_utils::kPgBoolOid, "f", 1) == json(false));
  REQUIRE(pg_value(pg_value_utils::kPgBoolOid, "", 0) == json(false));

  REQUIRE(pg_value(pg_value_utils::kPgInt2Oid, "7", 1) == json(7));
  REQUIRE(pg_value(pg_value_utils::kPgInt4Oid, "42", 2) == json(42));
  REQUIRE(pg_value(pg_value_utils::kPgInt8Oid, "9223372036854775807", 19) ==
          json(9223372036854775807LL));
  REQUIRE(pg_value(pg_value_utils::kPgOidOid, "26", 2) == json(26));
  REQUIRE(pg_value(pg_value_utils::kPgInt4Oid, "not-a-number", 12) ==
          json("not-a-number"));

  REQUIRE(pg_value(pg_value_utils::kPgFloat4Oid, "1.5", 3) == json(1.5));
  REQUIRE(pg_value(pg_value_utils::kPgFloat8Oid, "3.25", 4) == json(3.25));
  REQUIRE(pg_value(pg_value_utils::kPgNumericOid, "10.5", 4) == json(10.5));
  REQUIRE(pg_value(pg_value_utils::kPgFloat8Oid, "not-a-float", 11) ==
          json("not-a-float"));

  REQUIRE(pg_value(pg_value_utils::kPgTextOid, "hello", 5) == json("hello"));
  REQUIRE(pg_value(pg_value_utils::kPgVarcharOid, "world", 5) == json("world"));

  REQUIRE(pg_value(pg_value_utils::kPgByteaOid, "\\x48656c6c6f", 12) ==
          json("SGVsbG8="));
}

#include "pg_value_utils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace pg_value_utils;

TEST_CASE("pg_type_name maps every known OID and the unknown fallback") {
  REQUIRE(pg_type_name(kPgBoolOid) == "BOOLEAN");
  REQUIRE(pg_type_name(kPgByteaOid) == "BYTEA");
  REQUIRE(pg_type_name(kPgCharOid) == "CHAR");
  REQUIRE(pg_type_name(kPgNameOid) == "NAME");
  REQUIRE(pg_type_name(kPgInt2Oid) == "SMALLINT");
  REQUIRE(pg_type_name(kPgInt4Oid) == "INTEGER");
  REQUIRE(pg_type_name(kPgInt8Oid) == "BIGINT");
  REQUIRE(pg_type_name(kPgTextOid) == "TEXT");
  REQUIRE(pg_type_name(kPgOidOid) == "OID");
  REQUIRE(pg_type_name(kPgFloat4Oid) == "REAL");
  REQUIRE(pg_type_name(kPgFloat8Oid) == "DOUBLE PRECISION");
  REQUIRE(pg_type_name(kPgBpcharOid) == "BPCHAR");
  REQUIRE(pg_type_name(kPgVarcharOid) == "VARCHAR");
  REQUIRE(pg_type_name(kPgDateOid) == "DATE");
  REQUIRE(pg_type_name(kPgTimeOid) == "TIME");
  REQUIRE(pg_type_name(kPgTimestampOid) == "TIMESTAMP");
  REQUIRE(pg_type_name(kPgTimestamptzOid) == "TIMESTAMPTZ");
  REQUIRE(pg_type_name(kPgTimetzOid) == "TIMETZ");
  REQUIRE(pg_type_name(kPgNumericOid) == "NUMERIC");
  REQUIRE(pg_type_name(kPgJsonOid) == "JSON");
  REQUIRE(pg_type_name(kPgJsonbOid) == "JSONB");
  REQUIRE(pg_type_name(kPgUuidOid) == "UUID");
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
  REQUIRE(pg_value(kPgBoolOid, "t", 1) == json(true));
  REQUIRE(pg_value(kPgBoolOid, "f", 1) == json(false));
  REQUIRE(pg_value(kPgBoolOid, "", 0) == json(false));

  REQUIRE(pg_value(kPgInt2Oid, "7", 1) == json(7));
  REQUIRE(pg_value(kPgInt4Oid, "42", 2) == json(42));
  REQUIRE(pg_value(kPgInt8Oid, "9223372036854775807", 19) ==
          json(9223372036854775807LL));
  REQUIRE(pg_value(kPgOidOid, "26", 2) == json(26));
  REQUIRE(pg_value(kPgInt4Oid, "not-a-number", 12) == json("not-a-number"));

  REQUIRE(pg_value(kPgFloat4Oid, "1.5", 3) == json(1.5));
  REQUIRE(pg_value(kPgFloat8Oid, "3.25", 4) == json(3.25));
  REQUIRE(pg_value(kPgNumericOid, "10.5", 4) == json(10.5));
  REQUIRE(pg_value(kPgFloat8Oid, "not-a-float", 11) == json("not-a-float"));

  REQUIRE(pg_value(kPgTextOid, "hello", 5) == json("hello"));
  REQUIRE(pg_value(kPgVarcharOid, "world", 5) == json("world"));

  REQUIRE(pg_value(kPgByteaOid, "\\x48656c6c6f", 12) == json("SGVsbG8="));
}

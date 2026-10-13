# Round 100: покрытие — PG-хелперы конвертации вынесены в тестируемый заголовок

## Проблема

Чистые функции конвертации Postgres `pg_type_name`, `hex_to_bytes` и
`pg_value` жили в анонимном пространстве имён `db_query_executor_postgres.cpp`.
Юнит-тесты физически не могли их вызвать, поэтому их защитные ветки не
покрывались: разбор uppercase/невалидных hex-нибблов, fallback на строку при
неразбираемом числе (`std::stoll`/`std::stod` бросают), метка `UNKNOWN(oid)`,
пустой bytea и т.п. — 33 непокрытые строки в файле исполнителя.

## Решение

Функции вынесены в новый header-only `src/pg_value_utils.hpp` (пространство
имён `pg_value_utils`) — по тому же паттерну, что и `db_query_utils.hpp`
(«pure helpers … so the unit tests can exercise them»).

* OID-константы и параметры типизированы `uint32_t` (а не `Oid`), чтобы
  заголовок не тянул `libpq-fe.h` и оставался лёгким для тестов.
* `db_query_executor_postgres.cpp` включает заголовок и вызывает
  `pg_value_utils::pg_value` / `pg_value_utils::pg_type_name`; `PQftype(...)`
  приводится к `uint32_t` на месте. Лишние после переноса `#include` (base64,
  exception) убраны.

## Тесты

Новый `src/test_pg_value_utils.cpp` (добавлен в `test_components` в
`CMakeLists.txt`), три `TEST_CASE`:

* `pg_type_name` — все 22 известных OID + fallback `UNKNOWN(999999)`.
* `hex_to_bytes` — `\x`-форма, uppercase nibbles, нечётная длина, пустая
  строка, строка без `\x`-префикса, невалидные символы (→ нулевой ниббл).
* `pg_value` — bool (`t`/`f`/пусто), int2/int4/int8/oid, неразбираемое число
  → строка (catch), float4/float8/numeric (+ catch), text/varchar, bytea →
  base64.

## Результат

| Метрика | До | После |
| --- | --- | --- |
| Утверждения / тест-кейсы | 2585 / 556 | 2630 / 559 |
| Прод-строки (gсovr) | 6392/7133 = 89.61% | 6405/7135 = 89.77% |
| `db_query_executor_postgres.cpp` | 89.1% | 91.2% |
| `pg_value_utils.hpp` | — | 96.7% (88/91) |

Baseline `docs/coverage-baseline.json` обновлён (78 файлов, добавлен
`pg_value_utils.hpp`); остаток непокрытого в исполнителе — защитные ветки
живого соединения (недостижимы без сбоя сервера: `PQconnectdbParams` → null,
`PQnfields == 0`, сбой `statement_timeout` при ping).

## Проверка

* `./scripts/run-coverage.sh`: rc=0, гейт `--fail-under-line 90` пройден,
  per-file регрессий нет, «All tests passed» (2630/559).
* `./scripts/run-clang-tidy.sh` (изменённые): 0 предупреждений
  (в т.ч. `modernize-use-auto` — `const auto oid = static_cast<uint32_t>(...)`).
* `./scripts/run-cppcheck.sh`: 0 ошибок/предупреждений.
* `./rebuild-and-run.sh`: rc=0, тесты 2630/559.
* `./health-check.sh all`: all passed.
* `python3 message_counter.py --iterations 1 --concurrent 1`: success.

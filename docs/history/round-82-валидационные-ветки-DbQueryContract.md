# Round 82: валидационные ветки DbQueryContract (parse_db_query_request)

## Мотивация

`db_query_utils.hpp` — самый плотный по валидации заголовок DB-гейта:
`parse_db_query_request` (строки 88–141) умел среагировать на НЕ-объект,
неверный `type`, пустой `sql`, не-read-only SQL, нескалярные `params`,
некорректные `timeout_ms`/`max_rows` — но тесты гоняли только часть веток.
Разбор происходит ДО обращения к executor'у, поэтому вся таблица невалидных
запросов покрывается offline (без живой PostgreSQL) через
`DbQueryHandler::handle_request`.

## Изменения

`src/test_db_live.cpp` — новый тест `DbQueryHandler: rejects malformed
requests before dispatch` `[db]`:

* handler конструируется БЕЗ `init()` (валидация не доходит до executor-ов);
* массив `{json, ожидаемая_подстрока}` из 8 кейсов:
  * не-объект (`json::array({1})`) → "must be a JSON object";
  * `type="upsert"` → "type must be";
  * `query` без `sql` → "sql must be";
  * `DELETE FROM t` → "only read-only";
  * `params` = массив → "params must be an object";
  * `params` со вложенным массивом → "only scalar";
  * `timeout_ms=0` → "timeout_ms must be";
  * `max_rows=0` → "max_rows must be";
* каждый кейс: `status==400`, body `error`, подстрока в `message`.

## Результаты (coverage-образ)

| Метрика | До (round 81) | После |
| --- | --- | --- |
| Строки production | 6369/7113 = 89.54% | 6369/7113 = 89.54% |
| Ветви production | 6953/13411 = 51.85% | 6958/13411 = **51.88%** |
| `parse_db_query_request` L95/L104/L105/L116/L120/L134/L138 | частично | **на обе стороны** (L120 8/8, L134 4/4, L138 4/4) |

Нюанс: `db_query_utils.hpp` — inline-заголовок, инстанцируется в нескольких TU,
поэтому per-file ветви в gcovr слегка «плавают» (merging + `WARNING: negative
hits`, L126 4/8 — результат нескольких распространений). Нетто-дельты по
production: **+5 ветвей**, линии без изменений. Ценность теста не в цифрах, а в
явной фиксации валидационных контрактов (400 + BAD_REQUEST по 8 путям).

## Гейты

* coverage-образ зелёный; регресс-гейт обновлён после `--update` (допуск 0.0);
* clang-tidy по `test_db_live.cpp` — без замечаний.
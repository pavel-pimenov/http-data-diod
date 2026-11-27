# Раунд 274. Опциональные JSON-поля NATS-контракта через хелперы JsonUtils

Продолжение focused-рефакторинга после round 273: `JsonUtils` получил два
тонких хелпера `get_string_or()` / `get_bool_or()` — точные обёртки над
`nlohmann::json::value()`, без «безопасного» перехвата ошибок типа. Они
используются decoding-слоем worker и местом логики валидации запроса, где
`request_id` нужен только для Sentry-сообщения. Смысл: names key/fallback
собраны в одном месте, а контракт остался побайтово тем же.

## Что сделано

- `json_utils.hpp`: `get_string_or(j, key, fallback = "")` возвращает
  `j.value(key, std::string(fallback))`, `get_bool_or(j, key, fallback = false)`
  — `j.value(key, fallback)`. При отсутствии ключа используется fallback,
  пустая строка и `false` не подменяются, а значение неверного типа по-прежнему
  бросает `json::type_error`.
- `worker_request_parser.hpp`: семь обращений к `.value()` (query,
  body_is_binary, client_ip, proxy_ip, proxy_traceparent, proxy_span_id,
  traceparent) переведены на новые хелперы с теми же значениями по умолчанию.
- `l2_worker.cpp`: fallback `request_id` для Sentry-сообщения об ошибке
  валидации заменён на `JsonUtils::get_string_or()`.
- `sentry_client.cpp`, `trace_logger.cpp` и прочие `.value()`-вызовы не
  тронуты.

## Тесты

- `test_components.cpp`: прямые тесты хелперов проверяют попадание ключа,
  пустую строку, `false`, fallback для отсутствующего ключа и обязательный
  `json::type_error` на неверном типе (точная семантика `.value()`).
- Дополнительно в тесте optional-полей decoder'а теперь явно заданы пустые
  значения, а отдельный тест подтверждает, что числовой `client_ip` не
  подменяется `"unknown"`, а вызывает `json::type_error`.

## Проверки

- `./scripts/run-coverage.sh` и `python3 scripts/coverage-regression-check.py`
  — регрессий покрытия нет; baseline обновлён при необходимости.
- `./rebuild-and-run.sh`, `./health-check.sh all`,
  `python3 message_counter.py --iterations 1 --concurrent 1` — зелёные.
- `./scripts/ci-gate.sh offline` / `runtime` и `./scripts/run-cppcheck.sh` —
  зелёные; clang-tidy — только известные предупреждения из включаемых файлов
  (`json_utils.hpp:7` — неиспользуемый `<optional>`, `db_query_utils.hpp:51` —
  `std::min`).

Метрики, PromQL, README-каталог, golden-check, env и NATS-контракт не
изменялись.

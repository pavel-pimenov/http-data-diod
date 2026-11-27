# Раунд 273. Один хелпер для health status alive/ready

Продолжение focused-рефакторинга после round 272: два почти одинаковых
инлайн-хелпера `set_health_alive()` / `set_health_ready()` в
`json_response_utils.hpp` теперь вызывают одного внутреннего
`detail::set_health_status()`. Формат тела, статус и Content-Type не меняются.

## Что сделано

- Внутренний `detail::set_health_status(res, status, service)` собирает тот же
  `std::format(R"({{"status": "{}", "service": "{}"}})", ...)`, что использовали
  оба публичных хелпера: статус 200, `application/json`, пробелы после двоеточий
  и запятой сохраняются побайтово. JSON-escaping имени сервиса по-прежнему не
  производится — контракт health endpoints зависит от точного шаблона.
- `set_health_alive()` передаёт `"alive"`, `set_health_ready()` — `"ready"`.
  Все вызывающие точки (`request_handler.cpp`, `server_handler.cpp`, `main.cpp`)
  не менялись.

## Тесты

- `test_components.cpp`: два слабых `find()`-assert'а заменены точным сравнением
  bodies: `{"status": "alive", "service": "proxy"}` и
  `{"status": "ready", "service": "worker"}`.

## Проверки

- `./scripts/run-coverage.sh` и `python3 scripts/coverage-regression-check.py`
  — регрессий покрытия нет; baseline обновлён при необходимости.
- `./rebuild-and-run.sh`, `./health-check.sh all`,
  `python3 message_counter.py --iterations 1 --concurrent 1` — зелёные.
- `./scripts/ci-gate.sh offline` / `runtime`, `./scripts/run-clang-tidy.sh`,
  `./scripts/run-cppcheck.sh` — зелёные (без новых предупреждений).

Метрики, PromQL, README-каталог, golden-check, env и NATS-контракт не
изменялись.

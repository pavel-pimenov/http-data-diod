# Round 104: покрытие readiness-путей `RequestHandler` через live-брокер

## Что покрыто

`RequestHandler::handle_health_ready()` (`src/request_handler.cpp`) имел
непокрытыми ключевые ветви успеха:

* строка 170-171 — opt-in путь `HEALTH_READY_ALLOW_CONNECT=true`: при живом
  соединении `nats_healthy = (ping() == "PONG")`;
* строка 180 — путь по умолчанию: readiness читает только `is_connected()`, не
  инициируя реконнект;
* строки 197-201 — формирование ответа `200 {"status":"ready", ...}` и метрики
  `m_nats_connected=1` / `m_health_ready=1`.

Все существовавшие тесты readiness шли по offline-ветке (503
«NATS connection not available»), т.к. `init_proxy_components()` без брокера не
подключается.

## Тест

Новый `TEST_CASE("RequestHandler: readiness 200 with a live broker", "[nats-live]")`
в `src/test_nats_live.cpp` (по образцу соседнего DB-gateway live-теста):

* поднимает реальный `nats-server` (`NatsServer`), `live_proxy_env(...)` +
  `init_proxy_components(ctx)` → `REQUIRE(ctx.m_nats_client->is_connected())`;
* блок 1 (default): `HEALTH_READY_ALLOW_CONNECT=false`, `handle_get("/health/ready")`
  → `200`, `status=="ready"`, обе метрики `1.0`; проверяется, что флаг конфига
  действительно `false`;
* блок 2 (opt-in): `HEALTH_READY_ALLOW_CONNECT=true`, клиент подключён →
  `ping()=="PONG"` → `200`, `m_health_ready==1.0`.

Env собирается через `EnvVars vars = live_proxy_env(...); vars["..."] = ...;`
(прямой brace-init в `EnvGuard` с одним элементом не компилируется — С++
разбирает `{{"K","V"}}` как range-конструктор `map`; отмечено для будущих
тестов).

## Результат

| Метрика | До | После |
| --- | --- | --- |
| Утверждения / тест-кейсы | 2670 / 566 | 2683 / 567 |
| Прод-строки (gcovr) | 6435/7146 = 90.05% | 6442/7146 = 90.15% |
| `request_handler.cpp` строки | 405/449 | 412/449 |
| `request_handler.cpp` ветви | 483/961 | 497/961 |

Baseline `docs/coverage-baseline.json` обновлён (78 файлов, регрессий нет).

## Проверка

* `./rebuild-and-run.sh`: rc=0, «All tests passed» (2683 assertions in 567 test
  cases).
* `./health-check.sh all`: все проверки прошли.
* `python3 message_counter.py --iterations 1 --concurrent 1`: success.
* `./scripts/run-cppcheck.sh`: 0 ошибок/предупреждений.
* `./scripts/run-clang-tidy.sh` (изменённые): rc=0, 0 предупреждений.
* `./scripts/run-coverage.sh`: rc=0, гейты пройдены, per-file регрессий нет.

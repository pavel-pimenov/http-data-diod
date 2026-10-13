# Round 103: отдельный таймаут NATS-коннекта

## Проблема

Round 102 убрал бесконечную блокировку `NatsClient::connect()`
(`SetRetryOnFailedConnect(false)` + ограниченный цикл из 3 попыток). Но каждая
попытка всё ещё могла висеть до **полного request-таймаута**: `setup_options()`
ставил `natsOptions_SetTimeout(opts, ms_to_seconds(m_config.m_timeout_ms))`, а
`m_timeout_ms` берётся из `NATS_TIMEOUT_MS` (default **30000** мс) и предназначен
для ожидания ответа, а не для рукопожатия.

Для честно недоступного порта (connection refused) это неважно — ошибка приходит
мгновенно. Но для **black-hole** хоста (маршрут есть, SYN дропается/фильтруется
firewall'ом) `connect()` мог занять до ~30с × 3 попытки ≈ 90с — и
`publish`/`request`/`subscribe`/health через `ensure_connected()` → `connect(1)`
блокировались на этих таймаутах.

## Решение

Введён отдельный, меньший таймаут рукопожатия `NATS_CONNECT_TIMEOUT_MS`
(default **5000**):

1. **`src/config.hpp`** — `Config::Nats::m_connect_timeout_ms{5000}`.
2. **`src/config.cpp`** — чтение env `NATS_CONNECT_TIMEOUT_MS`; валидация
   `positive(...)` (сообщение `Invalid NATS_CONNECT_TIMEOUT_MS: {} (must be > 0)`);
   проброс в `create_nats_config()`.
3. **`src/nats_client.hpp`** — `NatsConfig::m_connect_timeout_ms = 5000`.
4. **`src/nats_client.cpp`** — `setup_options()` использует
   `natsOptions_SetTimeout(opts, ms_to_seconds(m_config.m_connect_timeout_ms))`.
5. **`docker-compose.yml`** — `NATS_CONNECT_TIMEOUT_MS=${NATS_CONNECT_TIMEOUT_MS:-5000}`
   в `l2-proxy` и `l2-worker` (единственные сервисы, использующие NATS).
6. **`.env.example`** — строка `NATS_CONNECT_TIMEOUT_MS=5000`.

Request-таймауты не затронуты: они передаются явно в
`natsConnection_RequestMsg(..., timeout_ms)`, а `m_timeout_ms` остаётся
fallback-значением для `request(..., 0)`.

## Тесты

* `Config: zero NATS connect timeout fails validation` — `m_connect_timeout_ms = 0`
  → `validate(false) == false` (`src/test_components.cpp`).
* `Config: create_nats_config maps NATS fields` — добавлен `EnvVarGuard` на
  `NATS_CONNECT_TIMEOUT_MS=2500` и `REQUIRE(nc.m_connect_timeout_ms == 2500)`.

## Результат

| Метрика | До | После |
| --- | --- | --- |
| Утверждения / тест-кейсы | 2668 / 565 | 2670 / 566 |
| Прод-строки (gcovr) | 6429/7140 = 90.04% | 6435/7146 = 90.05% |
| `config.cpp` строки | 562/568 | 568/574 |

Baseline `docs/coverage-baseline.json` обновлён (78 файлов, регрессий нет).

## Проверка

* `./rebuild-and-run.sh`: rc=0, «All tests passed» (2670 assertions in 566 test
  cases).
* `./health-check.sh all`: все проверки прошли.
* `python3 message_counter.py --iterations 1 --concurrent 1`: success.
* `./scripts/run-cppcheck.sh`: 0 ошибок/предупреждений.
* `./scripts/run-clang-tidy.sh` (изменённые): rc=0 (только неблокирующие
  `misc-include-cleaner`-предупреждения, предсуществующие).
* `./scripts/run-coverage.sh`: rc=0, гейты `--fail-under-line 90` /
  `--fail-under-branch 50` пройдены, per-file регрессий нет.

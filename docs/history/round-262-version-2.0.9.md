# Round 262 — Версия 2.0.9 + тест connect-timeout NATS (мс) + готовый прод-конфиг NATS

## Задача
- Добавить регрессионный тест на фикс connect-timeout NATS (round 261), чтобы
  баг `ms_to_seconds` не повторился.
- Подготовить готовый прод-конфиг NATS-сервера без временной диагностики
  (debug/trace).

## Реализация
- `src/test_nats_live.cpp`: добавлен тест
  `NatsClient: connect timeout is applied in milliseconds` (`[nats-live]`) —
  подключается к неотвечающему адресу (TEST-NET-1 `192.0.2.1`, unrouted) с
  `m_connect_timeout_ms = 1500` и проверяет, что connect реально ждёт ~1.5 с
  (не мгновенно из-за бага единиц). Добавлен helper
  `live_nats_cfg(subject, port, connect_timeout_ms)`.
- Прод-конфиг NATS (`/tmp/ks/nats.conf.final`): убраны `debug: true` и
  `trace: true` (временная диагностика), оставлены TLS (самоподписанные серты,
  `ca_file` — trust store, НЕ mTLS), `timeout: 5` на TLS-хендшейк.
- `README.md`: добавлена таблица "Переменные окружения NATS" с акцентом на то,
  что `NATS_CONNECT_TIMEOUT_MS` передаётся в `natsOptions_SetTimeout()` как мс
  (не конвертировать в секунды — баг `ms_to_seconds`, round 261).
- Проверены единицы всех NATS-таймаутов: `SetTimeout`/`DrainTimeout`/
  `ReconnectWait`/`RequestMsg` — все ожидают мс, воркер передаёт мс;
  `SetRetryOnFailedConnect` — отключён (nulls).
- `src/generate_version.sh`: `2.0.8-$SHA` → `2.0.9-$SHA`.

## Проверка
- Тест собран в builder-контейнере (`BUILD_TESTS=ON`), запущен:
  `All tests passed (5 assertions in 1 test case)`; connect к `192.0.2.1`
  ждал ~1.5 с (не мгновенно) — подтверждает корректный мс-таймаут.
- `./rebuild-and-run.sh` — сборка зелёная.

## Связь с фиксом (round 261)
- Корневая причина была: `natsOptions_SetTimeout` получал секунды (5) вместо
  мс (5000) через `ms_to_seconds()`, из-за чего connect-timeout был ~0 мс и
  кросс-хостовый connect мгновенно уходил в Timeout. Всё исправлено в 2.0.8,
  тест защищает от регрессии.

## Тэг
- `git tag -f 2.0.9 HEAD && git push origin 2.0.9` — тэг для CI-сборки.

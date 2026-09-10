# TODO / Продолжение работы

## Текущий статус (14 — покрытие ветвей расширено)

Raунды покрытия юнит-тестами: **364 + 74 = 438 test cases**,
**2 265+ assertions**, строковое покрытие **97.4%** (гейт 90%).
Последний раунд закрыл null-tracer guard ветки в `tracing_helpers.hpp`,
`set_db_pool_gauges` в `db_query_executor_base.cpp`, lowercase/alias ветки
в `logger.hpp`, и kMaxExpose cap в `rate_limiter_per_ip.hpp`.

E2E/fault-tolerance: **9 сценариев** (NATS reconnect, L2 server down, worker killed,
NATS dedup resend, proxy restart under load, multi-restart, concurrent restart,
drain, reply-loss).

## Куда дальше (по приоритету)

### 1. Дальнейшие раунды покрытия юнит-тестами

Оставшиеся пробелы по ветвям:
- `logger.hpp` — init-time ветки `LOG_LEVEL=CRITICAL/OFF` (env-dependent, требуют
  отдельного процесса с setenv перед init; std::call_once блокирует повторный вход)
- `trace_context_extractor.cpp` — non-null tracer paths (сложные, требуют мока
  JaegerLogger)
- `tracing_helpers.hpp` baggage-ветки ( Baggage::url_encode/url_decode уже
  покрыты через trace_logger tests)

### 2. Chaos-тестирование (fault_tolerance_test.py) — ГОТОВО

Все 9 сценариев реализованы и стабильны:
1. NATS reconnect
2. L2 server down
3. Worker killed
4. NATS dedup resend
5. Proxy restart under load
6. Multi-restart (3 последовательных)
7. Concurrent restart (worker + proxy одновременно)
8. Drain (docker compose stop)
9. Reply-loss (worker killed mid-flight)

### 3. Graceful shutdown — ГОТОВО

Реализовано:
- `InFlightTracker` + `server.stop()` + `wait_for_completion(30s)`
- `stop_grace_period: 40s` в docker-compose
- Worker: `ThreadPoolWrapper::reset()` + drain pool
- Метрика `l2_worker_graceful_shutdown_seconds` (gauge)
- Сценарий drain покрыт chaos-тестом #8

### 4. Self-hosted Sentry — ГОТОВО

Реализовано:
- `sentry-mock` сервис в `docker-compose.yml` (профиль `sentry-mock`)
- `scripts/sentry-mock-receiver.py` (HTTP-сервер, парсит envelope)
- `scripts/run-sentry-mock-stack.sh` (start/stop helper)
- `rebuild-and-run.sh` с `ENABLE_SENTRY_MOCK=true`
- Grafana-панель `l2-sentry-delivery` (uid) в `generate-grafana-dashboards.py`

### 5. Видимость кросс-сервисных метрик (архитектурное)

`l2_worker_sentry_*` и `l2_tracing_*` экспонируются только на 19091 (worker-режим).
В proxy/server режимах они не видны. Если нужна единая видимость — отдельный общий
реестр, экспонируемый на всех портах (широкое изменение).

## Замечание по окружению

NATS-ветка отключена — все изменения и тесты针对 NATS-режима.
Oracle-профиль запускается по demand: `docker compose --profile oracle up -d`.
Sentry-mock запускается по demand: `docker compose --profile sentry-mock up -d`.

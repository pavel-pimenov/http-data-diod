# TODO / Продолжение работы

## Текущий статус (15 — non-null JaegerLogger покрытие, замер покрытия)

Raунды покрытия юнит-тестами: **383 + 74 = 457 test cases**,
**2 311 assertions** (1568 + 743). Замер через `scripts/run-coverage.sh`
(gcovr в контейнере, HTML-отчёт в `coverage-report/`):
- **Lines: 97.4%** (8245/8465), гейт 90% — пройден
- **Functions: 95.0%** (1095/1153)
- **Branches: 41.8%** (16299/39038) — слабое место

Последний раунд: non-null JaegerLogger ветки в `tracing_helpers.hpp`
(теперь **99.2%**, 123/124 строк), `apply_traceparent`,
`begin_request_trace`, `make_span_and_traceparent`, `resolve_trace_id`
с реальным трейсером. Файлы <90% по строкам: `string_utils.hpp` (85.7%),
`http_client_pool.cpp` (88.4%), `trace_logger.cpp` (88.7%),
`duplicate_detector.cpp` (89.8%).

E2E/fault-tolerance: **9 сценариев** (NATS reconnect, L2 server down, worker killed,
NATS dedup resend, proxy restart under load, multi-restart, concurrent restart,
drain, reply-loss).

## Куда дальше (по приоритету)

### 1. Дальнейшие раунды покрытия юнит-тестами

Открытые ветки (строковое покрытие уже высокое, осталось ветвей):
- `logger.hpp` — init-time ветки `LOG_LEVEL=CRITICAL/OFF` (env-dependent, требуют
  отдельного процесса с setenv перед init; std::call_once блокирует повторный вход)
- `string_utils.hpp` (85.7%), `http_client_pool.cpp` (88.4%),
  `trace_logger.cpp` (88.7%), `duplicate_detector.cpp` (89.8%) — <90% по строкам
- **Branches 41.8%** в целом — крупный задел (gcovr `--branch` метрика), но
  ветви в header-heavy шаблонном коде требуют точечных тест-кейсов

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
- `glitchtip` сервис в `docker-compose.yml` (профиль `glitchtip`) + выделенный
  `glitchtip-db` (Postgres); образ `glitchtip/glitchtip:6`
- E2E-проверка доставки: `scripts/sentry-e2e-test.py` +
  `scripts/sentry-mock-receiver.py` (локальный mock-приёмник на хосте)
- `scripts/run-glitchtip-stack.sh` (start/stop helper)
- `rebuild-and-run.sh` с `ENABLE_GLITCHTIP=true`
- Grafana-панель `l2-sentry-delivery` (uid) в `generate-grafana-dashboards.py`

### 5. Видимость кросс-сервисных метрик (архитектурное)

`l2_worker_sentry_*` и `l2_tracing_*` экспонируются только на 19091 (worker-режим).
В proxy/server режимах они не видны. Если нужна единая видимость — отдельный общий
реестр, экспонируемый на всех портах (широкое изменение).

## Замечание по окружению

NATS-ветка отключена — все изменения и тесты针对 NATS-режима.
Oracle-профиль запускается по demand: `docker compose --profile oracle up -d`.
Sentry (glitchtip) запускается по demand: `docker compose --profile glitchtip up -d`.

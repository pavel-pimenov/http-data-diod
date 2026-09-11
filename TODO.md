# TODO / Продолжение работы

## Текущий статус (17 — все файлы ≥90% строкового покрытия)

Raунды покрытия юнит-тестами: **540 test cases**,
**2 524 assertions**. Замер через `scripts/run-coverage.sh`
(gcovr в контейнере, HTML-отчёт в `coverage-report/`):
- **Lines: 97.9%** (9032/9224), гейт 90% — пройден
- **Functions: 95.2%** (1188/1248)
- **Branches: 41.6%** (18237/43889) — слабое место

Последние раунды: доведение файлов ниже 90% строкового покрытия
до ≥90% (`duplicate_detector.cpp` 89.8%→94.5%,
`http_client_pool.cpp` 88.4%→90.1%, `trace_logger.cpp` 88.7%→90.2%);
`string_utils.hpp` — gcov-артефакт устранён выносом `to_lower` в
`string_utils.cpp` (100% строк; header больше не в отчёте).
Раунд ветвей: parse_url/format_http_error/JsonUtils/HeaderUtils/Gateway
routing + 12 кейсов валидации config.cpp. Вывод: каждая покрытая ветвь
в production добавляет 30-80 непокрываемых тест-ветвей макросов Catch2
(тест-файлы = 35290 из 42355 общего числа), поэтому общий % почти
недвижим; честная цель — production-only ветви 56.7% (3852/6791).
config.cpp: ветви валидации почти насыщены (общий `if (cond)` в
`ConfigChecker::check` — точка слияния), остальное — env-var ветки
`get_env_*` (override/invalid/default).
Раунд tracing: 13 кейсов в test_trace_logger.cpp — validate_traceparent
short-circuit (L68/L72 ☑), get_traceparent_header, begin_request_trace,
extract_and_validate, JaegerSpanLogger::log_l2_call/worker/proxy + null-трейсер,
log_nats_span, log_backend_error (detail-ветка ☑), rate_limit_rejection
(limit/remaining ☑), make_span_and_traceparent (hint/sampled/no-tracer),
add_proxy_trace_fields, set_traceparent_response_header, log_worker_span.
trace_logger.cpp 61.1%→61.5% (254→256/416), tracing_helpers.hpp 55.0%→55.6%
(155→158/284). Остаток — cross-TU-merge артефакты (test_proxy_core не
вызывает helper'ы) и sender_loop/retry/сетевые ветки.
Раунд send_envelope/stats_page: sentry_client.cpp 51.2%→53.2% (+13 ветвей),
stats_page.hpp 59.6%→62.6% (+11 ветвей), shutdown-flush в sender_loop (+2).
PROD branches 56.8%→57.2% (3893/6811).

E2E/fault-tolerance: **9 сценариев** (NATS reconnect, L2 server down, worker killed,
NATS dedup resend, proxy restart under load, multi-restart, concurrent restart,
drain, reply-loss).

## Куда дальше (по приоритету)

### 1. Дальнейшие раунды покрытия юнит-тестами

Открытые ветки (строковое покрытие уже высокое, осталось ветвей):
- `logger.hpp` — init-time ветки `LOG_LEVEL=CRITICAL/OFF` (env-dependent, требуют
  отдельного процесса с setenv перед init; std::call_once блокирует повторный вход)
- `sentry_client.cpp` — 53.2% ветвей (304 uncovered, network-heavy; закрыты
  http/https send_envelope failure-ветки, остаётся pool/процессные)
- `trace_logger.cpp`/`tracing_helpers.hpp` — 61.5%/55.6% (160+126 uncovered,
  in-process, достижимы без внешних сервисов)
- `stats_page.hpp` — 62.6% (140 uncovered, в осн. html-render артефакты:
  `s.m_points.empty()`, `family.metric.empty()` — не достижимы тестами)
- `string_utils.hpp` — gcov-артефакт: закрывающая `}` inline-функции в
  header-е (1 строка из 7), не устраним тестами; **РЕШЕНО** выносом в
  `string_utils.cpp` (100% строк, header без исполняемых строк)
- **Branches 41.5%** в целом — крупный задел (gcovr `--branch` метрика), но
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

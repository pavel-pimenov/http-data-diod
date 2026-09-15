# TODO / Продолжение работы

## Текущий статус (21 — чистки dead-code/констант; покрытие ≥90%)

Raунды покрытия юнит-тестами: **575 test cases**,
**2 719 assertions** (test_components 455/1836 + test_proxy_core 120/883). Замер через `scripts/run-coverage.sh`
(gcovr в контейнере, HTML-отчёт в `coverage-report/`):
- **Lines: 98.0%** (9752/9952), гейт 90% — пройден
- **Functions: 95.3%** (1244/1305)
- **Branches: 41.5%** (19652/47389) — слабое место

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

Раунды 19–21 (dead-code/константы, по результатам сканирования explore-агента):
- 19: удалён `TimeoutException` (не бросался нигде; 2 catch-сайта в
  request_handler были недостижимы), мёртвый `g_default_random_digits`,
  интервал статистики 600с → `kStatsLogIntervalSeconds` (статс-строка и
  `wait_for` ссылаются на одну константу).
- 20: дефолты `Config` — единственный источник в `config.hpp`: ~45 fallback-
  литералов `get_env_*("VAR", <lit>)` заменены на `m_field` (все сверены
  попарно; исключения: L2_SERVER_HOST, L2_SERVER_URLS/URL-поля,
  SENTRY_MAX_QUEUE_SIZE (size_t), весь DbConfig-блок).
- 21: `db_gateway_routing::kDbGatewayPath = "/v1/sql"` — единый корень шлюза
  (proxy роутинг + worker DB_execute span-name); параметры Jaeger-пула
  HttpClientPool → именованные константы (`kJaegerPoolMaxSize` и др.).
- М2/М3 (dedup-лимиты 4096/60000, tracing 50/1000) — **сознательно НЕ сделаны**:
  единый-source потребовал бы копил inclusive-header'ов (config.hpp ←
  dedup_cache.hpp/trace_logger.hpp), coupling дороже дрейфа двух литералов.
- Багgage-подсистема (DE2) — мертва в продакшене (~120 строк: `Baggage`,
  `TraceInfo`, `extract_trace_info`, `set/get/get_all_baggage`, thread-local
  TTL-карта, `SpanData::m_baggage`, `g_url_encode_hex`), прод-путь
  (`trace_context_extractor`/`tracing_helpers`) от неё независим. Подготовить
  как batch 22 (см. ниже), НЕ коммитить без `./rebuild-and-run.sh`.

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
- `stats_page.hpp` — 59.5% (378 ветвей, в осн. html-render артефакты:
  `s.m_points.empty()`, `family.metric.empty()`, extra-registry уже закрыт в
  раунде l2_common)
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

`l2_worker_sentry_*` и `l2_tracing_*` экспонируются на всех трёх портах
метрик (19090/19091/19092).

Реализовано: метрики перенесены из реестра воркера в общий реестр
`l2_common` (новый `AppContext::m_common_registry`), который подмешивается
к собственному реестру режима на каждом экспозере. `/stats` воркера и
прокси рендерят плитки `l2_common` дополнительно к своему реестру.

### 6. Batch 22 (подготовлен к реализации): удаление мёртвой Baggage-подсистемы

DE2 сканирования: `Baggage`, `TraceInfo`, `extract_trace_info`,
`set_baggage/get_baggage/get_all_baggage`, thread-local `g_trace_baggage` +
TTL-карта, `SpanData::m_baggage`, `g_url_encode_hex` — мертвы в продакшене
(единственные вызовы в `test_trace_logger.cpp`; прод-путь
`trace_context_extractor`/`tracing_helpers` от них не зависит). Удалять:

1. `src/trace_logger.hpp`: строки `g_url_encode_hex` (41), `struct TraceInfo`
   (43–52), `struct Baggage` (54–151), `TraceInfo extract_trace_info` decl
   (153–155), `Baggage m_baggage;` в `SpanData` (175), декларации
   set/get/get_all_baggage (290–294). После удаления проверить и снять
   лишние include: `<ranges>` (только из `from_header`), возможно
   `<unordered_map>`.
2. `src/trace_logger.cpp`: определение `extract_trace_info` (128–138) и блок
   Baggage Propagation (424–477). Aggregat-init `SpanData` (177) одной
   строкой меньше — корректно. `g_baggage_ttl_us`/`cleanup_expired_baggage`
   уходят вместе с блоком.
3. `src/test_trace_logger.cpp`: удалить ~49 упоминаний (тесты Baggage /
   extract_trace_info / url_encode-decode / set/get/get_all_baggage).

После удаления метрики не меняются (README не трогать). Обязателен
`./rebuild-and-run.sh` + `message_counter.py` перед коммитом.

## Заморожено (не делать, решение 2026-09-11)

- l2-server — не прод (тестовый стаб для worker): метрики/stats/дашборды/алерты
  не развивать, отдельный `/stats` и per-mode панели не делать (см. AGENTS.md).
- Branch-хвосты `sentry_client` (~53%), `trace_logger`/`tracing_helpers`
  (~55–61%), `stats_page` (~59%) дальше не гнать: общий % топчется из-за
  ветвей макросов Catch2 в тест-файлах. Держать гейт lines ≥90%.
- Переименование `l2_worker_sentry_*` → `l2_sentry_*` не делать (ломает
  дашборды; имя зафиксировано как legacy в общем реестре `l2_common`).
- Отложено до востребования: lazy-start 4× `MetricsHistory`, unit-тест
  `init_proxy_components()` с линковкой NATS, мелочи `test_app_context.cpp`
  (`AppCtxEnvGuard` restore старого значения MODE).

## Замечание по окружению

NATS-ветка отключена — все изменения и тесты только для NATS-режима.
Oracle-профиль запускается по demand: `docker compose --profile oracle up -d`.
Sentry (glitchtip) запускается по demand: `docker compose --profile glitchtip up -d`.

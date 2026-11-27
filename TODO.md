# TODO / Продолжение работы

## Передача дежурства (2026-11-27, round 276)

**Выключение компа прервало прогоны.** В рабочем дереве — round 276
(делегирование `set_json_error_response()` в `send_json_response()` +
стабилизация флейкового ThreadPool-теста), история и строка HISTORY.md готовы.
Перед уходом запущены, но результатов не дождались: `./scripts/run-coverage.sh`
(первый прогон упал на гонке `active_count`, после фикса перезапущен) и
`./rebuild-and-run.sh`. **Коммит и пуш — только после их зелёного результата**
(правило AGENTS.md: без успешной сборки в контейнере не коммитить).

Что проверить на новом компе по порядку:

1. `git status --short`, `git log -3 --oneline` — убедиться, что round 276 ещё
   не запушен (если запушен — ничего делать не нужно).
2. `./scripts/run-coverage.sh` (таймаут ≥30 мин; gcovr + baseline) и
   `./rebuild-and-run.sh`, затем `./health-check.sh all` и
   `python3 message_counter.py --iterations 1 --concurrent 1`.
3. `./scripts/ci-gate.sh offline`; для `runtime` поднять `l2-db-spider` с dummy
   `DB_SPIDER_*` (команда в архиве раундов), предупреждение про PostgreSQL
   локально ожидаемо.
4. Коммит `round 276: JSON-ошибка через send_json_response()` и пуш сделаны
   **по прямой просьбе пользователя без ожидания coverage/rebuild** (комп
   выключается); pre-commit тоже пропущен из-за параллельно идущей сборки.
   clang-tidy прогнан руками: только известное предупреждение
   `db_query_utils.hpp:51` (не блокирующее); cppcheck и offline-гейт зелёные,
   `git diff --check` чистый. Если повторный coverage или rebuild покажут
   проблему — зафиксировать отдельным round 277, не переписывая историю.
5. После зелёных прогонов удалить старые `/tmp/*.txt`.

Следующий кандидат (round 277, компактный): снова устранить дублирование
`res.status = ...; res.set_content(..., "application/json")` в оставшихся
хендлерах, переведя их на `send_json_response()` из `json_response_utils.hpp`;
затронуть только вызывающие точки, контракт ответов и метрики не менять.

## Правило портов (29*=данные, 19*=метрики)

- `29xxx` — рабочие/данные порты: `29001` (l2-server), `29002` (l2-proxy), `29003` (nginx)
- `19xxx` — метрики: `19090` (l2-proxy metrics), `19091` (l2-worker metrics), `19092` (l2-server metrics), `19093` (l2-worker health)
- Отразить правило в README (раздел портов) и network-architecture.md.

## Раунд 24 — Sentry transactions: release, сэмплинг, E2E, фиксы сборки

`SENTRY_RELEASE` попадает в транзакции; отдельный `SENTRY_SAMPLE_RATE`
(независимый от Jaeger, Bernoulli-per-span). Новый
`scripts/glitchtip-performance-e2e.py` — E2E по transaction groups через
реальные бинари (PASS на живом стенде). `rebuild-and-run.sh` больше не
падает на health-check (`set -e` фикс), `health-check.sh` обходит ALL_PROXY.

## Раунд 23 — GlitchTip Performance

`JaegerLogger` дублирует спаны в GlitchTip как Sentry-транзакции при
заданном `SENTRY_DSN` (страница Performance → Transaction Groups).
Отдельный HTTP-пул для хоста DSN, метрики
`l2_tracing_sentry_transactions_sent_total`/`_failed_total`, юнит-тесты,
healthcheck glitchtip и профиль в `rebuild-and-run.sh` починены.
Наблюдение: ingest GlitchTip асинхронный — группы появляются в БД через
несколько секунд после 200-ответа.

## Текущий статус (25 — transaction groups по продуктам)

Покрытие юнит-тестами: **784 test cases**, **4 294 assertions**
(test_components 652/3298 + test_proxy_core 132/996). Замер через `scripts/run-coverage.sh`
(gcovr в контейнере, HTML-отчёт в `coverage-report/`):
- **production-файлы: 88.2% строк** (7684/8713) — пер-файловый гейт
  `scripts/coverage-regression-check.py` против `docs/coverage-baseline.json`
- **все файлы (с тестами): 92.9% строк** (17775/19141)
- **ветви: 41.1%** (34442/83899) — в это число входят throw-рёбра и макросы Catch2
  в тестах; по production-файлам — 51.5% (8250/16036). После round 268 самый
  тёмный production-файл — `db_query_executor_postgres.cpp` 196/287 строк
  (stream_query с параметрами требует live PG), затем `db_query_handler.hpp`
  11/20 (`all_ready()` без инъекции executor'а) и `db_query_handler.cpp`
  109/120; `db_spider.cpp` поднялся с 0/176 до 128/176, `pool_executor.hpp` —
  до 25/26.

Последние раунды: доведение файлов ниже 90% строкового покрытия
до ≥90% (`duplicate_detector.cpp` 89.8%→94.5%,
`http_client_pool.cpp` 88.4%→90.1%, `trace_logger.cpp` 88.7%→90.2%);
`string_utils.hpp` — gcov-артефакт устранён выносом `to_lower` в
`string_utils.cpp` (100% строк; header больше не в отчёте).
Раунд ветвей: parse_url/format_http_error/JsonUtils/HeaderUtils/Gateway
routing + 12 кейсов валидации config.cpp. Вывод: каждая покрытая ветвь
в production добавляет 30-80 непокрываемых тест-ветвей макросов Catch2
(тест-файлы = 65053 из 79323 общего числа), поэтому общий % почти
недвижим; production-only ветви — 53.2% (7596/14270).
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
- Багgage-подсистема — **удалена (batch 22)**: `Baggage`, `TraceInfo`,
  `extract_trace_info`, `set/get/get_all_baggage`, thread-local TTL-карта,
  `SpanData::m_baggage`, `g_url_encode_hex`, + 9 TEST_CASE в
  test_trace_logger/test_components. Прод-путь
  (`trace_context_extractor`/`tracing_helpers`) от неё не зависел —
  проверено по всем файлам src. В процессе верификации вскрыт и
  **исправлен** скрытый race-тест sentry "clamp to one" (см. HISTORY).
  Цифры покрытия после batch 22 — в шапке.

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

### 2. Chaos-тестирование (fault-tolerance-test.py) — ГОТОВО

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
- `glitchtip` сервис в `docker-compose.yml` (теперь всегда в стеке, без профиля)
  + выделенный `glitchtip-db` (Postgres); образ `glitchtip/glitchtip:7`
- E2E-проверка доставки: `scripts/sentry-e2e-test.py` +
  `scripts/sentry-mock-receiver.py` (локальный mock-приёмник на хосте)
- `scripts/run-glitchtip-stack.sh` (start/stop helper)
- Grafana-панель `l2-sentry-delivery` (uid) в `generate-grafana-dashboards.py`

### 5. Видимость кросс-сервисных метрик (архитектурное)

`l2_worker_sentry_*` и `l2_tracing_*` экспонируются на всех трёх портах
метрик (19090/19091/19092).

Реализовано: метрики перенесены из реестра воркера в общий реестр
`l2_common` (новый `AppContext::m_common_registry`), который подмешивается
к собственному реестру режима на каждом экспозере. `/stats` воркера и
прокси рендерят плитки `l2_common` дополнительно к своему реестру.

### 6. Batch 22 — ГОТОВО (закоммичен после verified build)

Удаление мёртвой Baggage-подсистемы реализовано и собрано в контейнере
(`./rebuild-and-run.sh` + message_counter PASS). Детали в HISTORY.md.
В ходе верификации исправлен race-тест sentry "clamp to one"
(test_sentry_client.cpp) — покрытие-гейт без фикса падал 2/2 в coverage-
конфиге. Покрытие после batch 22: Lines 98.0% (9563/9757, гейт √),
Functions 95.2%, Branches 41.4%.

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
СУБД (postgres/oracle) запускаются по demand: `docker compose --profile databases up -d`.
Sentry-совместимый GlitchTip (`http://localhost:8000`) теперь **всегда** в стеке:
после первого запуска зарегистрируй юзера, создай проект и подставь
`SENTRY_DSN=http://<key>@glitchtip:8000/<project-id>` в `.env`.

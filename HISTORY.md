# История проекта (указатель)

> Полные детали каждого раунда вынесены в `docs/history/` — здесь только указатель. Новый раунд: заведи подробную запись в `docs/history/round-NNN-<тема>.md` и одну строку-ссылку в этот файл.

| Раунд | Тема | Детали |
| --- | --- | --- |
| 73 | флак nats_client закрыт (baseline 359→385/457), production-only гейт ветвей ≥45% (третья gcovr-инвокация с исключением test_*.cpp), новые live-тесты auth/TLS/connect-bootstrap/пустой-reply и реконнект L2Worker после рестарта брокера — строки 93.62%, ветви 41.20% (полный) / 51.73% (production) | [round-73](docs/history/round-73-nats-покрытие-production-гейт-ветвей-45.md) |
| 72 | живой PostgreSQL в юнит-тестах (test_db_live.cpp: initdb-кластер в builder, PgServer) — db_query_executor_postgres 19.4%→88.5% строк/53.3% ветвей, db_query_handler 66.3%→97.6%; gcovr-фикс negative hits; зафиксирован флак nats_client ±10 строк; строки 93.37%, ветви 41.15% | [round-72](docs/history/round-72-живой-PostgreSQL-в-юнит-тестах-db-live.md) |
| 71 | живой nats-server в юнит-тестах (test_nats_live.cpp: NatsClient pub/sub/reconnect, PollService, DB-gateway, L2Worker run-loop, fail-fast offline) — NATS-покрытие: nats_client 80.7%, poll 88%, строки 92.3%, ветви 41.1% | [round-71](docs/history/round-71-живой-NATS-брокер-в-юнит-тестах-nats-live.md) |
| 70 | гейт ветвей 40% + пер-файловый гейт регрессии покрытия (coverage-regression-check.py ↔ docs/coverage-baseline.json) | [round-70](docs/history/round-70-гейт-ветвей-40-процентов-и-пер-файловая-регрессия-покрытия.md) |
| 69 | покрытие юнит-тестами до 90% (тесты линкуют весь бинарник, новые test_l2_worker/test_proxy_handlers, StatsLogger с параметром периода) + гейт --fail-under-line 90 | [round-69](docs/history/round-69-покрытие-тестами-90-процентов-гейт-fail-under-line-90.md) |
| 68 | живое сравнение Grafana ↔ Perses (новый runtime-гейт) + анализ покрытия юнит-тестами | [round-68](docs/history/round-68-живое-сравнение-Grafana-Perses-в-гейте-и-анализ-покрытия.md) |
| 67 | Perses в гейтах: offline --check конвертера + metrics-consistency (Grafana == Perses), фикс SC2034, README-раздел | [round-67](docs/history/round-67-perses-в-offline-гейт-и-metrics-consistency-гигиена-и-README.md) |
| 66 | Perses — лёгкая альтернатива Grafana: контейнер в стеке + нативный конвертер дашбордов (fallback — миграция через /api/migrate) | [round-66](docs/history/round-66-perses-лёгкая-альтернатива-grafana-контейнер-и-нативный-конвертер-дашбордов.md) |
| 65 | гигиена скриптов: chmod +x, NOEOL, shebang, set -euo pipefail, lint-python в offline-гейт, shellcheck-гейт | [round-65](docs/history/round-65-гигиена-скриптов-chmod-x-noeol-shebang-set-euo-pipefail-lint-python-shellcheck.md) |
| 64 | единый CI-гейт (ci-gate.sh), env-var ↔ compose гейт, docker build-context гейт, кэш docker-слоёв в CI | [round-64](docs/history/round-64-единый-CI-гейт-env-па-и-docker-build-context-гейты-кэш.md) |
| 63 | CI-фиксы: clang-tidy версионного заголовка на хосте, dockerignore generate_version.sh | [round-63](docs/history/round-63-CI-фиксы-clang-tidy-dockerignore-версионный-заголовок.md) |
| 62 | гейт метрик DB Gateway end-to-end, clang-tidy sweep, сжатие HISTORY.md | [round-62](docs/history/round-62-гейт-метрик-DB-Gateway-clang-tidy-и-сжатие-HISTORY.md) |
| 61 | гейт метрик — обратное направление (дашборды ↔ живой /metrics) + юнит-тесты | [round-61](docs/history/round-61-гейт-метрик-обратное-направление-дашборды-живой-metrics-юнит.md) |
| 60 | гейт сверки имён метрик (C++ ↔ дашборды ↔ README ↔ golden-check) | [round-60](docs/history/round-60-гейт-сверки-имён-метрик-C-дашборды-README-golden-check.md) |
| 59 | микро-метрики round 57 в дашбордах и golden-check | [round-59](docs/history/round-59-микро-метрики-round-57-в-дашбордах-и-golden-check.md) |
| 58 | civetweb-заглушки убраны из вендоренного дерева (генерируются CMake), поправка round 56 | [round-58](docs/history/round-58-civetweb-заглушки-убраны-из-вендоренного-дерева-генерируются.md) |
| 57 | микро-метрики proxy (task queue vs NATS poll) — локализация роста p99 | [round-57](docs/history/round-57-микро-метрики-proxy-task-queue-vs-NATS-poll-локализация-рост.md) |
| 56 | обновление вендоренных библиотек (cpp-httplib 0.59.0, prometheus-cpp master) | [round-56](docs/history/round-56-обновление-вендоренных-библиотек-cpp-httplib-0-59-0-promethe.md) |
| 55 | e2e reconnect worker + dedup-replay, error-contract, perf-baseline, non-root hardening | [round-55](docs/history/round-55-e2e-reconnect-worker-dedup-replay-error-contract-perf-baseli.md) |
| 54 | юнит-тесты для worker/NATS-контракта, добивка покрытия | [round-54](docs/history/round-54-юнит-тесты-для-worker-NATS-контракта-добивка-покрытия.md) |
| 53 | покрытие, clang-tidy sweep, лёгкие тесты/чистка | [round-53](docs/history/round-53-покрытие-clang-tidy-sweep-лёгкие-тесты-чистка.md) |
| 52 | ускорение сборки (PCH для l2-proxy, параллельные юнит-тесты, apt-кэш) | [round-52](docs/history/round-52-ускорение-сборки-PCH-для-l2-proxy-параллельные-юнит-тесты-ap.md) |
| 51 | обновление вендорных либ (cpp-httplib 0.58.0, nats.c 3.14.0) | [round-51](docs/history/round-51-обновление-вендорных-либ-cpp-httplib-0-58-0-nats-c-3-14-0.md) |
| 50 | группировка оставшихся членов Config (App/Proxy/Server/Worker) | [round-50](docs/history/round-50-группировка-оставшихся-членов-Config-App-Proxy-Server-Worker.md) |
| 49 | группировка членов Config (RateLimit/Dedup/Duplicate) | [round-49](docs/history/round-49-группировка-членов-Config-RateLimit-Dedup-Duplicate.md) |
| 48 | группировка членов Config (DbQuery) | [round-48](docs/history/round-48-группировка-членов-Config-DbQuery.md) |
| 47 | группировка членов Config (Tracing/Sentry) | [round-47](docs/history/round-47-группировка-членов-Config-Tracing-Sentry.md) |
| 46 | группировка членов Config (Nats/Ssl) | [round-46](docs/history/round-46-группировка-членов-Config-Nats-Ssl.md) |
| 45 | группировка глобальных констант histogram_buckets (Buckets) | [round-45](docs/history/round-45-группировка-глобальных-констант-histogram_buckets-Buckets.md) |
| 44 | группировка приватных членов RAII-профайлеров и DbRowCollector (State) | [round-44](docs/history/round-44-группировка-приватных-членов-RAII-профайлеров-и-DbRowCollect.md) |
| 43 | группировка приватных членов ThreadPoolWrapper (Backend) + ScopedRequestContext (State) | [round-43](docs/history/round-43-группировка-приватных-членов-ThreadPoolWrapper-Backend-Scope.md) |
| 42 | группировка приватных членов JaegerLogger (Pools/Delivery) | [round-42](docs/history/round-42-группировка-приватных-членов-JaegerLogger-Pools-Delivery.md) |
| 41 | группировка приватных членов L2Worker (Clients/State) | [round-41](docs/history/round-41-группировка-приватных-членов-L2Worker-Clients-State.md) |
| 40 | группировка приватных членов RequestHandler (Config/Services) | [round-40](docs/history/round-40-группировка-приватных-членов-RequestHandler-Config-Services.md) |
| 39 | группировка приватных членов NatsClient (Connection/State) | [round-39](docs/history/round-39-группировка-приватных-членов-NatsClient-Connection-State.md) |
| 38 | группировка приватных членов OracleQueryExecutor (Init) + LogContextScope (Previous) | [round-38](docs/history/round-38-группировка-приватных-членов-OracleQueryExecutor-Init-LogCon.md) |
| 37 | группировка приватных членов DbQueryHandler (State) + RetryHandler (Config/State) | [round-37](docs/history/round-37-группировка-приватных-членов-DbQueryHandler-State-RetryHandl.md) |
| 36 | группировка приватных членов DynamicLabeledFamily (Config/State) + CircuitBreaker (Counters) | [round-36](docs/history/round-36-группировка-приватных-членов-DynamicLabeledFamily-Config-Sta.md) |
| 35 | группировка приватных членов MetricsHistory (Config/Store/Sampler) + DuplicateDetector (State) | [round-35](docs/history/round-35-группировка-приватных-членов-MetricsHistory-Config-Store-Sam.md) |
| 34 | группировка приватных членов SentryClient (Config/Metrics/QueueState) + ThreadPool (Workers/Queue) | [round-34](docs/history/round-34-группировка-приватных-членов-SentryClient-Config-Metrics-Que.md) |
| 33 | группировка приватных членов HttpClient (Config/Transport/ConnState) + DedupCache (Config/State) | [round-33](docs/history/round-33-группировка-приватных-членов-HttpClient-Config-Transport-Con.md) |
| 32 | (fixed+): завершение группировки ResponseValidator — приватные члены в Allowed/Limits (m_allowed/m_limits) | [round-32](docs/history/round-32-завершение-группировки-ResponseValidator-приватные-члены-в-A.md) |
| 29 | завершение группировки приватных членов HttpClientPool — getters + cpp (PoolConfig/ConnState/Counters) | [round-29](docs/history/round-29-завершение-группировки-приватных-членов-HttpClientPool-gette.md) |
| 28a | группировка приватных членов вложенными struct в NatsClient/JaegerLogger/RateLimiter*/CrashHandler (аудит + завершение миграции, 0 «плоских» членов) | [round-28a](docs/history/round-28a-группировка-приватных-членов-вложенными-struct-в-NatsClient-.md) |
| 28 | финальный аудит группировки вложенных struct (JaegerLogger/RateLimiter/RateLimiterPerIP/NatsClient/CrashHandler) — тела методов переведены на m_config.*/m_frames.*/m_sentry.* | [round-28](docs/history/round-28-финальный-аудит-группировки-вложенных-struct-JaegerLogger-Ra.md) |
| 27 | интеграция настроек предохранителя трассировки (TracingBreakerSettings) + coverage метрик в дашбордах Grafana | [round-27](docs/history/round-27-интеграция-настроек-предохранителя-трассировки-TracingBreake.md) |
| 26 | фикс core-дампа при сборке + доделка прерванной ветки изменений | [round-26](docs/history/round-26-фикс-core-дампа-при-сборке-доделка-прерванной-ветки-изменени.md) |
| 25 | разделение transaction groups по продуктам (MODE) | [round-25](docs/history/round-25-разделение-transaction-groups-по-продуктам-MODE.md) |
| 24 | Sentry transactions — release, отдельный sample rate, E2E, фикс health-check | [round-24](docs/history/round-24-Sentry-transactions-release-отдельный-sample-rate-E2E-фикс-h.md) |

### Раунды 23 и старше

История до round 24 (включая batch/chore/refactor-правки и служебные раунды `30+`, `32`, затерянные в хронологии) хранится единым массивом в [docs/history/archive-pre-round-24.md](docs/history/archive-pre-round-24.md).

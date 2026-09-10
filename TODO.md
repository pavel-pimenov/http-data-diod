# TODO / Продолжение работы

## Текущий статус (13e — покрытие 97.4%, golden-check стабилен)

Раунды покрытия юнит-тестами доведены до **13e**: 344 + 74 = 418 test cases,
2 213 assertions, строковое покрытие **97.4%** (гейт 90%).

E2E/fault-tolerance: 5 сценариев (NATS reconnect, L2 server down, worker killed,
NATS dedup resend, proxy restart under load).

## Куда дальше (по приоритету)

### 1. Chaos-тестирование (fault_tolerance_test.py)

Текущие 5 сценариев покрывают остановку сервисов, но не покрывают:

- **Race при параллельном рестарте** worker и proxy одновременно (rolling restart)
- **Медленный drain NATS** — `docker compose stop` с увеличенным `stop_grace_period`,
  чтобы проверить, что drain действительно ждёт in-flight сообщения
- **Потеря reply_to** — симулировать потерю NATS reply (удалить подписку proxy
  на reply subject mid-flight), проверить таймаут и retry
- **Сетевая задержка** — через `tc netem` добавить delay/packet-loss между контейнерами,
  проверить что circuit breaker срабатывает и сервис не зависает
- **Множественные рестарты** — 3 последовательных рестарта worker с интервалом 5s,
  проверить что dedup-кэш не теряется и сообщения не дублируются

### 2. Graceful shutdown — проверка дрейна NATS/DB при SIGTERM

Текущая реализация:
- Proxy: `InFlightTracker` + `server.stop()` + `wait_for_completion(30s)`,
  `stop_grace_period: 40s` в docker-compose
- Worker: `ThreadPoolWrapper::reset()` (деструктор ждёт завершения всех задач),
  `stop_grace_period: 40s`
- NATS: `NatsClient::drain()` + `NatsClient::unsubscribe()` в деструкторе

Что нужно проверить/доработать:

- **Интеграционный тест graceful shutdown**: отправить 10+ запросов параллельно,
  вызвать `docker compose stop l2-worker`, убедиться что:
  1. Новые запросы отклоняются (503/not-ready)
  2. In-flight запросы завершаются (не обрываются)
  3. NATS drain завершается до SIGKILL (40s > drain timeout)
  4. Лог содержит "Graceful shutdown" и "All in-flight requests completed"
- **Метрика drain time**: добавить gauge `l2_worker_graceful_shutdown_seconds`
  (время от SIGTERM до полного завершения) для мониторинга в проде
- **DB-пулы**: проверить что при shutdown worker'а oracle/postgres соединения
  корректно закрываются (не остаются в ESTABLISHED в `ss -tnp`)

### 3. Self-hosted Sentry — mock-приёмник в compose

Текущий статус:
- `sentry-mock` сервис есть в `docker-compose.yml` (профиль `sentry-mock`),
  использует `scripts/sentry-mock-receiver.py` (HTTP-сервер, парсит envelope)
- E2E тест `scripts/sentry-e2e-test.py` проверяет доставку
- `SENTRY_DSN` передаётся в оба сервиса (proxy, worker)
- `rebuild-and-run.sh` умеет поднимать mock вместе со стеком опционально:
  `ENABLE_SENTRY_MOCK=true ./rebuild-and-run.sh` (выставляет `SENTRY_DSN`,
  добавляет `--profile sentry-mock`)

Что ещё можно доработать:

- **Скрипт-хелпер**: `scripts/run-sentry-mock-stack.sh` — поднять mock +
  пересоздать только proxy/worker с `SENTRY_DSN` (без полной пересборки)
- **Grafana-панель Sentry**: пока метрики `l2_worker_sentry_*` ненулевые —
  добавить дашборд через `generate-grafana-dashboards.py`

### 4. Обновить README.md

- Обновить счётчики тестов (418 test cases, 2 213 assertions)
- Добавить описание chaos-тестов (когда будут реализованы)
- Обновить раздел fault-tolerance (текущие 5 сценариев + новые)

### 5. Дальнейшие раунды покрытия юнит-тестами

Слабейшие по ветвям модули:
- `logger.hpp` ~47% ветвей (нужны отдельные процессы с разным env для init-веток)
- `db_query_executor_base.cpp` ~42% (тяжело мокать без реальной БД)
- `tracing_helpers.hpp` ~58% (ветки baggage-обработки)
- `rate_limiter_per_ip.hpp` ~59% (ветки get_per_ip_stats + TTL-ветки)

### 6. Видимость кросс-сервисных метрик (архитектурное)

`l2_worker_sentry_*` и `l2_tracing_*` экспонируются только на 19091 (worker-режим).
В proxy/server режимах они не видны. Если нужна единая видимость — отдельный общий
реестр, экспонируемый на всех портах (широкое изменение).

## Замечание по окружению

NATS-ветка отключена — все изменения и тесты针对 NATS-режима.
Oracle-профиль запускается по demand: `docker compose --profile oracle up -d`.
Sentry-mock запускается по demand: `docker compose --profile sentry-mock up -d`.

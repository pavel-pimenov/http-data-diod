# TODO / Продолжение работы

## Текущий статус (10c-10d — Sentry: DB-гейтвей + e2e доставки)

Раунд **10c** завершён:

- DB-гейтвей (воркер): Sentry-capture 503/500 ошибок
  (`fingerprint {db_query_error, code}`, теги `db`/`type`) + INTERNAL_ERROR.
- Аудит env: config.cpp ↔ docker-compose.yml — висячих переменных нет
  (все get_env_* присутствуют; инфраструктурные переменные используются).

Раунд **10d** завершён:

- E2E реальной доставки Sentry: `scripts/sentry-e2e-test.py` +
  `scripts/sentry-mock-receiver.py` — mock Sentry-ingest на 9001; остановка
  l2-worker → `proxy_backend_error` (empty_response, 504) → mock принимает
  событие с корректным fingerprint/envelope. Доставка через асинхронную
  очередь + httplib подтверждена.
- Метрики `l2_worker_sentry_*` видны на `19091/metrics` (worker реестр);
  в proxy/server режимах не экспонируются — общий паттерн с `l2_tracing_*`
  (задокументировано в README).

## Куда дальше (по приоритету)

1. **Sentry — self-hosted приёмник в compose (опционально)**: локальный
   mock уже доказал доставку; если нужен постоянный стенд — поднять
   self-hosted Sentry (или оставить mock-скрипты для CI). Проверить панель
   Grafana для `l2_worker_sentry_*`.
2. **Видимость кросс-сервисных метрик (архитектурное)**: `l2_worker_sentry_*`
   и `l2_tracing_*` не экспонируются в proxy/server режимах (только 19091).
   Если нужна единая видимость — регистрировать общие метрики в отдельном
   общем реестре и экспонировать его на всех портах (широкое изменение).
3. **Дальнейшие раунды покрытия юнит-тестами**:
   - `string_utils`/`json_response_utils`/`error_types` — уже покрыты напрямую.
   - Остаются тяжёлые модули: `nats_client.cpp`, AppContext-методы
     (NATS/БД-зависимые, в `test_components` не втащить — искать новые
     dependency-light функции).
4. **README/дашборды**: каталог метрик актуален (проверено: все `l2_*` из
   кода есть в каталоге; висячих нет).
5. **Мусор сессии**: `/tmp/opencode/` пуст; удалить rebuild-*.log /
   clang-tidy-*.log, если появятся.

## Замечание по окружению

Во время 10a исправлен несовместимый с `postgres:17-alpine` data-dir
(инициализирован PG16): удалён named volume `http-data-diod_postgres-data`,
`postgres` поднят заново (healthy), схема из `sql/postgres.sql` применена
повторно. Если следующий запуск даст ошибку «database files are incompatible» —
просто удалить volume ещё раз.
# TODO / Продолжение работы

## Текущий статус (10b — Sentry: точки захвата)

Раунд **10b «Sentry — расширение точек захвата»** завершён:

- l2-proxy: capture в `fail_backend_request` (queue_failed / timeout /
  empty_response / invalid_response) — fingerprint `{proxy_backend_error,
  category}`, тег `request_id`.
- l2-server: capture ошибки валидации тела — fingerprint
  `{server_validation_error, schema}`.
- `SKIP_CLANG_TIDY=1` в `scripts/pre-commit.sh` — опциональный пропуск
  дорогого clang-tidy-гейта (ручной прогон: `./scripts/run-clang-tidy.sh`).

## Куда дальше (по приоритету)

1. **Sentry — реальная доставка**: `SENTRY_DSN` нигде не задан → клиент в
   no-op. Проверить живую отправку на реальный DSN (self-hosted Sentry в
   compose или внешний) и что события доходят; при необходимости добавить
   панель Grafana для `l2_worker_sentry_*`.
2. **Дальнейшие точки захвата**: при необходимости — ошибки DB-гейтвея
   (db_query_handler), декомпрессия, NATS-подключение.
3. **Дальнейшие раунды покрытия юнит-тестами**:
   - `string_utils`/`json_response_utils`/`error_types` — уже покрыты напрямую.
   - Остаются тяжёлые модули: `nats_client.cpp`, AppContext-методы
     (NATS/БД-зависимые, в `test_components` не втащить — искать новые
     dependency-light функции).
4. **README/дашборды**: пройтись по каталогу метрик — убедиться, что
   `l2_worker_sentry_*` отображаются и что семейства с метками
   (`status`, `db`, `type`, `ip`, `client_id`, `state`) описаны корректно.
5. **docker-compose**: проверить висячие переменные окружения (после
   удаления Redis-ветки и добавления Sentry-переменных) — по правилу
   «environment ↔ config.cpp».
6. **Мусор сессии**: удалить временные файлы в `/tmp/opencode/`
   (rebuild-9s*.log, clang-tidy-9*.log).

## Замечание по окружению

Во время 10a исправлен несовместимый с `postgres:17-alpine` data-dir
(инициализирован PG16): удалён named volume `http-data-diod_postgres-data`,
`postgres` поднят заново (healthy), схема из `sql/postgres.sql` применена
повторно. Если следующий запуск даст ошибку «database files are incompatible» —
просто удалить volume ещё раз.
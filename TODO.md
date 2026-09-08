# TODO / Продолжение работы

## Текущий статус (10c — Sentry: DB-гейтвей + аудит env)

Раунд **10c** завершён:

- DB-гейтвей (воркер): Sentry-capture 503/500 ошибок
  (`fingerprint {db_query_error, code}`, теги `db`/`type`) + INTERNAL_ERROR.
- Аудит env: config.cpp ↔ docker-compose.yml — висячих переменных нет
  (все get_env_* присутствуют; инфраструктурные переменные используются).

## Куда дальше (по приоритету)

1. **Sentry — реальная доставка**: `SENTRY_DSN` нигде не задан → клиент в
   no-op. Проверить живую отправку на реальный DSN (self-hosted Sentry в
   compose или внешний) и что события доходят; при необходимости добавить
   панель Grafana для `l2_worker_sentry_*`.
2. **Sentry — e2e-проверка захвата**: запустить стек с mock/self-hosted
   Sentry-receiver и убедиться, что capture из воркера/прокси/сервера
   реально доставляется (метрики `l2_worker_sentry_*` растут).
3. **Дальнейшие раунды покрытия юнит-тестами**:
   - `string_utils`/`json_response_utils`/`error_types` — уже покрыты напрямую.
   - Остаются тяжёлые модули: `nats_client.cpp`, AppContext-методы
     (NATS/БД-зависимые, в `test_components` не втащить — искать новые
     dependency-light функции).
4. **README/дашборды**: пройтись по каталогу метрик — убедиться, что
   `l2_worker_sentry_*` отображаются и что семейства с метками
   (`status`, `db`, `type`, `ip`, `client_id`, `state`) описаны корректно.
5. **Мусор сессии**: `/tmp/opencode/` пуст (чистка не требуется); если
   появятся rebuild-*.log / clang-tidy-*.log — удалить.

## Замечание по окружению

Во время 10a исправлен несовместимый с `postgres:17-alpine` data-dir
(инициализирован PG16): удалён named volume `http-data-diod_postgres-data`,
`postgres` поднят заново (healthy), схема из `sql/postgres.sql` применена
повторно. Если следующий запуск даст ошибку «database files are incompatible» —
просто удалить volume ещё раз.
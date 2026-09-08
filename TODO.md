# TODO / Продолжение работы

## Текущий статус (10a — Sentry)

Раунд **10a «Интеграция с Sentry»** завершён и запушен:

- Лёгкий Sentry-клиент `cpp/l2-proxy/sentry_client.{hpp,cpp}` (без `sentry-native`,
  на `cpp-httplib`): DSN-парсер, event/envelope JSON, async `jthread` + bounded-queue,
  метрики sent/failed/queue_size, инжектируемый transport для тестов.
- Конфиг: `SENTRY_DSN`, `SENTRY_ENVIRONMENT`, `SENTRY_RELEASE`,
  `SENTRY_TIMEOUT_MS`, `SENTRY_MAX_QUEUE_SIZE` (добавлены в docker-compose.yml
  для l2-server / l2-proxy / l2-worker).
- Интеграция в `l2_worker.cpp`: capture при ошибке валидации схемы и при
  исчерпании попыток вызова L2-сервера.
- README.md: раздел «Отслеживание ошибок (Sentry, воркер)» + метрики
  `l2_worker_sentry_*`.
- Проверено: clang-tidy чисто, сборка в контейнере ✅ (1204/295 + 742/73),
  e2e message_counter ✅.

## Где прервался

Все проверки ради 10a зелёные, коммит+push выполнен. Дальнейшая работа
не начиналась.

## Планы дальше (по приоритету)

1. **Sentry — реальная доставка**: сейчас `SENTRY_DSN` нигде не задан →
   клиент работает в no-op режиме. Проверить живую отправку на реальный DSN
   (self-hosted Sentry в compose или внешний) и что события доходят до
   проекта/проекта-приёмника; при необходимости добавить панель в дашборд
   Grafana для `l2_worker_sentry_*`.
2. **Sentry — расширение точек захвата**: рассмотреть добавление ошибок
   HTTP-запросов (логирование ошибок l2-proxy/l2-server), возможно через
   существующую вспомогательную функцию инициализации.
3. **Дальнейшие раунды покрытия юнит-тестами** (как 9a–9c):
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
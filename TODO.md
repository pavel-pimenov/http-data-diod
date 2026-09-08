# TODO / Продолжение работы

## Текущий статус (10e — юнит-тест полного SentryEvent)

Раунды **10c**/**10d** завершены (см. HISTORY.md). Раунд **10e**:

- `test_sentry_client.cpp`: прямой тест пути `SentryClient::capture(SentryEvent)`
  с тегами/extra/fingerprint (используется DB-гейтвеем из 10c) — проходят.

## Куда дальше (по приоритету)

1. **Дальнейшие раунды покрытия юнит-тестами**: чистые утилиты
   (`json_utils`, `string_utils`, `time_utils`, `common_utils`,
   `db_query_utils`, `error_types`, `stats_page`, Sentry) уже покрыты
   напрямую. Остаются тяжёлые модули (`nats_client.cpp`, AppContext-методы,
   приватный `trim_copy` в executor’е) — непросто втащить в юнит-тесты.
   Кандидат: опциональный ENABLE_COVERAGE-сбор и сводный gcovr (lcov +
   genhtml) по свежим .gcno/.gcda из builder-контейнера.
2. **Sentry — self-hosted приёмник в compose (опционально)**: доставка
   доказана mock-скриптами (sentry-e2e-test.py); для постоянного стенда
   поднять self-hosted Sentry или прикрутить mock к CI. Панель Grafana для
   `l2_worker_sentry_*` — только когда метрики станут ненулевыми на стенде.
3. **Видимость кросс-сервисных метрик (архитектурное)**: `l2_worker_sentry_*`
   и `l2_tracing_*` экспонируются только на 19091 (worker-реестр). В
   proxy/server режимах они не видны. Если нужна единая видимость —
   отдельный общий реестр, экспонируемый на всех портах (широкое изменение).
4. **README/дашборды**: каталог метрик сверен с кодом (все l2_* из кода
   есть в каталоге). Специального Sentry-дашборда нет — метрики доставки
   мониторятся через каталожные панели при ненулевом DSN.
5. **Мусор сессии**: `/tmp/opencode/` пуст; временные файлы удалены.

## Замечание по окружению

Во время 10a исправлен несовместимый с `postgres:17-alpine` data-dir
(инициализирован PG16): удалён named volume `http-data-diod_postgres-data`,
`postgres` поднят заново (healthy), схема из `sql/postgres.sql` применена
повторно. Если следующий запуск даст ошибку «database files are incompatible» —
просто удалить volume ещё раз.
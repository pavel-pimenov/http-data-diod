# Автономный бэклог (ночной прогон)

Правила: только first-party `src/*.cpp|hpp`; не трогать Oracle, l2-server, redis,
вендорные `httplib`/`prometheus-cpp`/`json`/`base64`; не гнать замороженные
branch-хвосты (`sentry_client`, `trace_logger`, `tracing_helpers`, `stats_page`).
Каждый пункт = отдельный раунд: код + тесты → `./rebuild-and-run.sh` →
(если меняются тесты/покрытие) `./scripts/run-coverage.sh` +
`coverage-regression-check.py --report coverage-report/cov.json --update` →
`health-check.sh all` + `message_counter.py --iterations 1 --concurrent 1` →
`ci-gate.sh all` → коммит с записью в `docs/history/round-NNN-*.md`, строкой в
`HISTORY.md` и обновлением цифр в `TODO.md`.

## Открытые

1. **213 — закрыт.** Унификация JSON-ошибок (`set_json_error_response`),
   убрана двойная установка `status` в DB-gateway, константы `kNotReady*`
   для причин not-ready, тест `DuplicateDetector::eviction_reason_name`.
2. **214 (в работе)** — вынести оценку готовности `handle_health_ready` в
   чистый тестируемый хелпер `readiness::evaluate_nats(...)` (сейчас ветви
   `ping_failed` / `health_check_failed` требуют живого NATS и почти не
   покрыты): вход — флаги ping/health/connected, выход — структура
   `{healthy, reason, error}`; обработчик только логирует и пишет ответ.
   Плюс кейсы на все комбинации в `test_proxy_core`/`test_components`.
3. **215 — закрыт.** Строгий разбор TCP-порта (`port_utils::parse_tcp_port`)
   в `parse_url` и `SENTRY_DSN`, чистый разбор DSN в `crash_dsn.hpp`.
4. **216** — обновление вендорных либ по запросу пользователя: `nats.c`
   3.14.0→3.15.0, `cpp-httplib` 0.63.0→0.67.0, `odpi` 26.0.0→26.1.0
   (остальные три уже на последних апстримных пинах); апдейт пинов в скрипте
   и `src/VENDORED-LIBS.md`, полная проверка сборки и пуш по просьбе пользователя.
5. **217** — `db_gateway_routing`: 34/34 строк и нужные ветви 404/405 уже
   покрыты — пункт заменить на анализ свежих дублей/длинных функций после 216.
6. **218** — `utf8_utils`: добить краевые ветви санитайзера (overlong,
   surrogate, обрезка на границе, > U+10FFFF) — только line/branch внутри
   допустимого.
7. **219** — проход `clang-tidy`/`cppcheck` по изменённым файлам, мелкие
   modernize/readability правки без изменения поведения.

## Правила выбора следующего пункта

Берётся первый невыполненный пункт. Если он упирается в окружение (нет Docker
и т.п.) — пропустить и перейти к следующему, записав причину. Если все пункты
закрыты — провести короткий анализ кода (grep дублей, длинные функции) и
добавить 2–3 новых пункта.

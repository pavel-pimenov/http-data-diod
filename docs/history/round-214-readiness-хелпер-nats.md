# round 214 — ready-контракт NATS вынесен в чистый тестируемый хелпер

## Что было не так

`RequestHandler::handle_health_ready()` держал всю матрицу ветвей проверки NATS
внутри обработчика: «клиента нет», «клиент есть, но соединения нет», default-путь
без пинга, opt-in путь `HEALTH_READY_ALLOW_CONNECT` с `ping()`, плюс catch на
исключение. Управлять этими ветвями из юнит-тестов можно было только живым
брокером (`test_nats_live.cpp`), поэтому `ping_failed` и `health_check_failed`
в обычном прогоне не исполнялись, а сам выбор причины отказа (метка
`l2_proxy_not_ready_total{reason}`) не был зафиксирован ни одним быстрым тестом.

Заодно константы причин (`kNotReady*`) и список `g_not_ready_reasons` жили в
тяжёлом `app_context.hpp`, который тянет `config.hpp`, Prometheus и `AppContext`:
подключить их в лёгкий `test_proxy_core` было нельзя.

## Что сделано

- Новый заголовок `src/readiness.hpp`:
  - константы `kNotReadyNatsUnavailable` / `kNotReadyPingFailed` /
    `kNotReadyHealthCheckFailed` и собираемый из них `g_not_ready_reasons`
    переехали сюда из `app_context.hpp` (тот теперь только включает заголовок);
  - `readiness::NatsProbe` — снимок проверок (`m_has_client`, `m_allow_connect`,
    `m_connected`, `m_ping_ok`) без `NatsClient`/`AppContext`;
  - `readiness::evaluate_nats(probe)` — чистая функция, возвращающая
    `Result{m_ready, m_reason, m_error}`: нет клиента/нет соединения →
    `nats_unavailable` + «NATS connection not available»; opt-in путь с
    неудачным пингом → `ping_failed` + «NATS ping failed»; иначе ready;
  - `readiness::nats_check_failed(what)` — маппинг исключения в
    `health_check_failed` + «NATS health check failed: …».
- `handle_health_ready()` теперь только собирает `NatsProbe` (вызовы
  `is_connected()`/`ping()` остались в try), вызывает хелпер, инкрементит
  `m_not_ready_total` по `result.m_reason` и отдаёт ответ через
  `write_nats_readiness()`. Все прежние комментарии сохранены.
- Тесты в `src/test_proxy_core.cpp` (лёгкий TU, без брокера): отсутствующий
  клиент, отключённый клиент на обоих путях (в т.ч. что `m_ping_ok` не влияет),
  connected без пинга, allow-connect с успешным и неуспешным пингом, маппинг
  исключения, и проверка, что каждая причина хелпера входит в
  `g_not_ready_reasons` (серия метрики предсоздана).

Поведение не изменилось: те же статусы, тела и значения меток, что и до
рефакторинга (live-тесты `test_nats_live.cpp` проходят как раньше).

## Проверка

- `./rebuild-and-run.sh` — сборка и запуск стека зелёные, `test_components`
  632 кейса / 3088 assertions, `test_proxy_core` 131 / 976 (5 новых кейсов).
- `./scripts/run-coverage.sh` + `coverage-regression-check.py` — гейты 90% строк /
  40% ветвей пройдены; production-файлы **7144/7770 = 91.94%** (91 файл,
  добавился `readiness.hpp`), `readiness.hpp` — 14/14 строк; регрессий нет,
  baseline обновлён.
- `./health-check.sh all`, `python3 message_counter.py --iterations 1 --concurrent 1`,
  `./scripts/ci-gate.sh all` — rc=0.

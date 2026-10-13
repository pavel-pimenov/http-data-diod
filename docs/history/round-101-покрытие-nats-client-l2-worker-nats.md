# Round 101: покрытие — error-пути NatsClient и DB-хендлер воркера

## Проблема

После round 100 оставались два самых «тонких» по покрытию прод-файла:

* `nats_client.cpp` — 72 непокрытые строки, в основном error/guard-ветви
  `request`/`subscribe`/`mark_disconnected`;
* `l2_worker_nats.cpp` — 46 непокрытых, в т.ч. ветка «DB-запрос до готовности
  шлюза».

Существующие live-тесты (`test_nats_live.cpp`) проверяли таймаут и невалидную
подписку через **сырой C-API libnats** (`natsConnection_RequestString`,
`natsConnection_Subscribe`), а не через `NatsClient`, поэтому error-пути самого
объекта не набирались.

## Решение

Таргетные тесты через публичный API `NatsClient` (а не «сырой» libnats) и
`L2WorkerTestAccess`.

### `src/test_nats_live.cpp` — 3 live-теста

* `NatsClient: request timeout marks the connection down and records the
  error` — подписчик получает запрос, но не отвечает → `NATS_TIMEOUT` (не
  `NATS_NO_RESPONDERS`), клиент помечается нездоровым
  (`mark_disconnected`, `get_last_error`). Набирает `request_impl` 474-476 и
  else-ветвь `mark_disconnected` 789.
* `NatsClient: request with timeout 0 falls back to the config timeout` —
  `request(..., 0)` берёт `m_config.m_timeout_ms` (тернарник 462) и получает
  ответ от `NatsResponder`.
* `NatsClient: subscribe rejects an invalid subject` — `subscribe("invalid
  subject with spaces")` → `NATS_INVALID_SUBJECT` (ветви 571-573).

## Тест воркера

Новый `TEST_CASE` в `src/test_l2_worker.cpp`: воркер создаётся без `run()`,
поэтому `m_db_query_handler` ещё не инициализирован. Валидный DB-запрос через
`L2WorkerTestAccess::process_db_query_from_nats` уходит в ветвь 573-576
(`503 DB_UNAVAILABLE`, «DB gateway is not initialized»).

## Обнаруженная ловушка (не тест)

Промежуточный тест «health check при упавшем брокере» (стоп nats-server, затем
`check_connection()`) **вешал весь прогон**: `check_connection` → `ensure_connected`
→ `connect()`, а `connect()` при недоступном брокере крутит `while (true)` со
`sleep(1s)` (сетевые логи видны только на первой итерации — `first_attempt`).
Пришлось удалить тест: ветви `check_connection` 645-651 в тестах недостижимы по
дизайну `connect()`. (Отдельно: строки-«артефакты» gcov на мног строковых
`Logger::info/warn` — первая физическая строка вызова показывает `count=0`,
хотя код исполнялся; такие строки не «добиваются» тестами.)

## Результат

| Метрика | До | После |
| --- | --- | --- |
| Утверждения / тест-кейсы | 2630 / 559 | 2651 / 563 |
| Прод-строки (gcovr) | 6405/7135 = 89.77% | 6416/7135 = 89.92% |
| `nats_client.cpp` (непокрыто) | 72/457 | 64/457 |
| `l2_worker_nats.cpp` (непокрыто) | 46/362 | 43/362 |

Baseline `docs/coverage-baseline.json` обновлён (78 файлов). Остаток
непокрытого — защитные/race-ветки (`acquire_connection` → null, сбой создания
NATS-сообщения/заголовков, `catch` вокруг публикации, `disconnect`/`drain`
при живом соединении) и gcov-артефакты мног строковых `Logger`-вызовов.

## Проверка

* `./scripts/run-coverage.sh`: rc=0, гейт `--fail-under-line 90` пройден,
  per-file регрессий нет, «All tests passed» (2651/563).
* `./scripts/run-clang-tidy.sh` (изменённые): 0 предупреждений.
* `./scripts/run-cppcheck.sh`: 0 ошибок/предупреждений.
* `./rebuild-and-run.sh`: rc=0.
* `./health-check.sh all`: all passed.
* `python3 message_counter.py --iterations 1 --concurrent 1`: success.

# Round 102: фикс бесконечной блокировки `NatsClient::connect()` при недоступном брокере

## Проблема

Round 101 обнаружил реальную ловушку: тест «health check при упавшем брокере»
(стоп `nats-server`, затем `check_connection()`) вешал весь прогон. Тогда тест
просто удалили. В этом раунде причина разобрана до конца и исправлена.

## Диагностика

`connect()` вызывал `natsConnection_Connect()` под `SetRetryOnFailedConnect(true)`.
Воспроизведение на бинарнике `nats-server`-образа (`http-data-diod:builder`)
показало (пробник линкуется с `/usr/local/lib/libnats.a`):

```
retry=1: natsConnection_Connect() → БЛОКИРУЕТСЯ навсегда (не возвращает управление)
retry=0: natsConnection_Connect() → status=6 (No server available) за ~50 мс
```

То есть с `RetryOnFailedConnect(true)` **`natsConnection_Connect()` никогда не
возвращает управление**, пока брокер недоступен. Поскольку вызов делается под
`m_connection.m_mutex`, `connect()` вешал не только свой поток, но и блокировал
любую другую NATS-операцию (мьютекс не отпускается). Проявлялось это на
`ensure_connected()` → `connect()`, то есть в `publish`/`request`/`subscribe`/
`check_connection`/`ping`.

Существующий `while (true)` retry-цикл в `connect()` был, по сути, мёртвым кодом
в сценарии «сервер недоступен» — до него управление не доходило. Offline-тесты
обходили проблему трюком: подсовывали несуществующий TLS-CA
(`/nonexistent-test-ca.pem`), из-за чего `setup_options()` падал и `connect()`
возвращал `false` до сетевого вызова.

## Решение

Три изменения в `src/nats_client.cpp` / `src/nats_client.hpp`:

1. **`setup_options()`: `SetRetryOnFailedConnect(false)`.** Теперь
   `natsConnection_Connect()` быстро возвращает `NATS_NO_SERVERS`, а retry берёт
   на себя цикл в `connect()`. Авто-реконнект уже установленного соединения
   от этого флага не зависит (его по-прежнему обеспечивают
   `AllowReconnect(true)` + `MaxReconnect(-1)`).
2. **`connect(int max_attempts = kDefaultConnectAttempts)`** — цикл стал
   ограниченным (`kDefaultConnectAttempts = 3`) и по-прежнему прерывается по
   `m_state.m_shutdown`. При каждой итерации мьютекс берётся и отпускается
   (sleep 1с — без удержания). Сбой `setup_options()` (битый CA и т.п.) завершает
   попытку сразу, без расходования попыток. По исчерпании — `false` + лог.
3. **`ensure_connected()` вызывает `connect(1)`** — одна попытка, чтобы
   `publish`/`request`/`subscribe`/`check_connection` имели ограниченную задержку,
   когда брокер лежит; длительное восстановление ведут внешние циклы (worker
   run-loop, poll service) со своим backoff.

Контракт `check_connection()`/`ping()` не менялся (health-эндпоинт по-прежнему
может инициировать ограниченный реконнект при `HEALTH_READY_ALLOW_CONNECT=true`).

## Тесты (`src/test_nats_live.cpp`)

* `NatsClient: connect fails fast when the broker is unreachable` — рабочий, но
  никем не слушаемый порт: `connect()` возвращает `false` за ~2с (3 попытки),
  публичный API (`check_connection`/`ping`/`publish`) тоже быстро возвращает
  `false`/`nullopt`. Прямой регресс-гард на бесконечный блок.
* `NatsClient: health check fails fast once the broker stops` — старт брокера,
  коннект, `server.stop()`, ожидание `!is_connected()`, затем
  `check_connection()`/`ping()` должны ответить быстро (<10с). Это тот самый
  сценарий из round 101, который раньше вешал прогон.

## Результат

| Метрика | До | После |
| --- | --- | --- |
| Утверждения / тест-кейсы | 2651 / 563 | 2668 / 565 |
| Прод-строки (gcovr) | 6416/7135 = 89.92% | 6429/7140 = 90.04% |
| `nats_client.cpp` строки | 393/457 | 406/462 |
| `nats_client.cpp` ветви | 362/756 | 382/760 |
| `l2_worker_nats.cpp` | 319/362 | 319/362 |

Baseline `docs/coverage-baseline.json` обновлён (78 файлов).

## Проверка

* Прямой репро поведения nats.c в builder-образе (см. выше) — фикс подтверждён.
* `./rebuild-and-run.sh`: rc=0, «All tests passed» (2668 assertions in 565 test
  cases).
* `./health-check.sh all`: все проверки прошли.
* `python3 message_counter.py --iterations 1 --concurrent 1`: success.
* `./scripts/run-cppcheck.sh`: 0 ошибок/предупреждений.
* `./scripts/run-clang-tidy.sh` (изменённые): rc=0 (только неблокирующие
  `misc-include-cleaner`-предупреждения в `nats_client.hpp`, предсуществующие).
* `./scripts/run-coverage.sh`: rc=0, гейты `--fail-under-line 90` /
  `--fail-under-branch 50` пройдены, per-file регрессий нет.

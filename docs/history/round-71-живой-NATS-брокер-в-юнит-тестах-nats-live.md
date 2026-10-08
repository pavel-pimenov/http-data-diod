# Round 71: живой NATS-брокер в юнит-тестах (покрытие NATS-стека)

## Мотивация

Round 70 поставил глобальные гейты (строки 90%, ветви 40%) и пер-файловую
регрессию, но всё NATS-покрытие опиралось на fail-fast трюк с несуществующим
TLS-CA файлом (connect обрывается до диалога) — а значит часть торгового цикла
была физически недостижима:

- `nats_client.cpp` — 295/457 строк, ветви 16.5%: настоящие колбэки
  `on_disconnected`/`on_reconnected`, `request_with_consume_span_id` с headers,
  `drain`/`ping`/`check_connection`, `mark_disconnected` по успешному коннекту;
- `l2_worker_nats.cpp` — 90/361 строк: `send_l2_response` 1/29, `run_with_nats`
  (цикл подписки), dedup, `send_nats_response_impl`;
- `nats_poll_service.cpp` — 70/125: `poll_response` (успех и no-responders),
  `poll_ensure_connected`;
- DB-gateway ветки 200/502/504 в `request_handler.cpp`.

Раунд приносит живой `nats-server` прямо в юнит-тесты.

## Изменения

### 1. `src/Dockerfile` — бинарь `nats-server` в builder-стадию

Стадия `builder` (а от неё наследуется `coverage`) тянет бинарь из того же
образа, что крутит композный брокер:

```dockerfile
COPY --from=nats:2.14-alpine /usr/local/bin/nats-server /usr/local/bin/nats-server
```

BuildKit резолвит `--from` под архитектуру билда, поэтому в многоарх-окружении
(arm64-машина + arm64-образ, amd64-CI и т.п.) в образ попадает правильный ELF.
Никакие runtime-стадии не затронуты: тесты живут только в builder/coverage.

### 2. `src/test_nats_live.cpp` (новый) — `[nats-live]` / `[nats-offline]`

Фикстура `NatsServer`: резервирует свободный loopback-порт, `fork`+`execlp`
(логи в `/tmp/nats-test-<port>.log`, `setsid`), ждёт готовности TCP-пробой
(бounded), на деструктор — `SIGTERM` с `waitpid` и эскалацией на `SIGKILL`.
`restart()` гонит падение/поднятие брокера на том же порту. Бинаря нет в
окружении → тесты `SKIP` (поиск по фиксированным путям и по `PATH`), поэтому
локальные прогоны без docker-образа не ломаются. Все синхронизации —
`wait_for_condition` с дедлайнами, ни один тест не может повесить сборку.

Тест-кейсы:

- **NatsClient pub/sub + headers + queue** — подписка с колбэком, 3 сообщения +
  publish_with_headers, queue-group из двух подписчиков (30 сообщений),
  unsubscribe.
- **request/reply + headers + consume span id** — `request()`,
  `request_with_headers()` (извлечение ключей из reply), `request_with_consume_span_id()`
  (заголовок `X-Consume-Span-Id`), `check_connection()`, `ping()`, `drain(3000)`.
- **reconnect после рестарта брокера** — `server.restart()`, ожидание
  `is_connected()` (nats.c автоматический reconnect), повторный pub/sub; живые
  колбэки `on_disconnected`/`on_reconnected`/`mark_disconnected`.
- **NatsPollService** — успешный round-trip (ответчик отвечает envelope +
  consume-span header), retry-цикл на пустых ответах (`poll_notify_resend`,
  `poll_delay_for_empty_reply` else-ветка, `poll_sleep_backoff(250)`),
  ветка «No responders available» (`poll_sleep_backoff(1000)`).
- **DB gateway через RequestHandler** — три контекста proxy на живом брокере:
  валидный envelope → 200, не-JSON ответ → 502, тихий брокер (нет ответчика) → 504.
- **L2Worker run-loop** — `worker.run()` в потоке, sleep-loop подписки, тестовый
  клиент делает `request()` в subject воркера: успех (envelope 200 c
  `l2-live-val`), повторный запрос с тем же request_id → **dedup-cache** (L2
  бэкенд получает ровно один POST), не-JSON payload → `{"error":...}`.
- **offline fail-fast (`[nats-offline]`, без брокера)** — весь публичный API
  несоединённого клиента (publish/subscribe/request/check_connection/ping/drain/
  unsubscribe/disconnect) + `NatsPollService::poll_response` в
  `poll_ensure_connected` fail-ветке.

### 3. `src/CMakeLists.txt` — новый тестовый TU в `test_components`

`test_nats_live.cpp` добавлен в список исходников `test_components` рядом с
`test_l2_worker.cpp`; coverage-стадия подхватывает его автоматически.

### 4. `docs/coverage-baseline.json` — пересобрана база

`--update` принял новый production-файл `http_client.cpp` (раньше 0 покрытых
строк и не попадал в baseline — теперь 95.7%, его закрыл live-прогон
`L2Worker`) — всего 77 файлов.

## Цифры (свежая coverage-сборка)

| Метрика | Было (round 70) | Стало |
| --- | --- | --- |
| Строки (весь проект) | 13092/14505 = 90.3% | 13958/15124 = **92.3%** |
| Ветви (весь проект) | 24987/61521 = 40.6% | 26268/63908 = **41.1%** |
| Ветви (production без `test_*.cpp`) | 6236/13271 = 47.0% | 6635/13407 = 49.5% |
| Строки production-файлов (baseline) | 5689/7018 = 81.06% | 6089/7112 = 85.62% |

Ключевые файлы:

| Файл | Было (строк) | Стало |
| --- | --- | --- |
| `nats_client.cpp` | 162/457 (35.5%), ветви 16.5% | 369/457 (80.7%), ветви 41.1% |
| `l2_worker_nats.cpp` | 271/361 (75.1%) | 293/361 (81.2%) |
| `nats_poll_service.cpp` | 55/125 (44.0%), ветви 23.5% | 110/125 (88.0%), ветви 41.2% |
| `nats_push_service.cpp` | 27/29 (93.1%) | 27/29 (93.1%) — остаток continuation-строки |
| `http_client.cpp` | 77/94 (новое в baseline) | 90/94 (95.7%) |

## Проверки

- `./scripts/run-coverage.sh` — **rc=0**: сборка проходит оба гейта gcovr
  (строки 92.3 ≥ 90, ветви 41.1 ≥ 40), regression-скрипт `OK` после `--update`;
- полный прогон `test_components`: **531 тест-кейс / 2324 assertion, все зелёные**
  (в том числе live-тесты против nats-server v2.14.3 внутри coverage-образа);
- `./scripts/ci-gate.sh all`, clang-tidy по новым файлам, pre-commit.

## Как дальше

- порог ветвей 40 → 45: резерв по-прежнему дают трое «живых» — Oracle/Postgres
  экзекуторы и `db_query_handler.cpp` (им нужны реальные СУБД, это e2e-территория);
- оставшийся жир `l2_worker_nats.cpp` (68 строк) и `nats_client.cpp` (88 строк) —
  это в основном continuation-строки логов и нечастые ветки (таймауты ретраев).
# Round 81: live NATS+PostgreSQL комбо-тест worker-а и poll-ветки

## Мотивация

Round 74 оставил **незавершённую оснастку**: `pgtest::PgSrv` в
`test_nats_live.cpp`, `db_handler_ready()` в `l2_worker.hpp` и тег
`[db-live-combo]` — но сам тест проверял только `is_nats_connected()`,
а вся PostgreSQL-часть была мёртвой. Заодно оставался редко нагруженный
прод-контур `L2Worker::process_db_query_from_nats` (362-строка worker-пути),
который работал только в связке NATS→worker→PostgreSQL→reply.

## Изменения

### 1. `src/test_nats_live.cpp` — комбо-тест доведён до работающего

Тест `L2Worker: serves DB queries over live NATS+Postgres` `[db-live-combo]`
теперь поднимает **и** `NatsServer`, **и** `pgtest::PgSrv`:

* `DB_QUERY_ENABLED=true` + `DB_POSTGRES_ENABLED=true` + `DB_POSTGRES_{HOST,
  PORT,DB,USER}` из живого PgSrv; `DB_QUERY_NATS_SUBJECT` — отдельный subject;
* worker запускается `run()` в фоне (тот же паттерн, что у main-subject теста);
* **query**: `build_db_query_request("query", ..., "postgres", {"sql":
  "SELECT 42 AS answer"})` публикуется на DB-subject, ответ ждём bounded-ретраем
  через `wait_for_condition`; проверяется HTTP-envelope `status=200`, body
  `ok` + колонка `answer/INTEGER` + `rows=[[42]]` + `row_count=1`;
* **ping**: тот же path с типом `ping` → `status=200`, `ok` + `latency_ms`;
* после успешного ответа `REQUIRE(worker.db_handler_ready())` — публичный
  предикат round 74 наконец используется.

Новые включают: `db_query_utils.hpp` (для `build_db_query_request`).

### 2. `src/test_nats_live.cpp` — poll-ветки

Два маленьких теста на недопокрытые ветки `nats_poll_service.cpp`:

* `NatsPollService: returns empty when the NATS client is missing`
  `[nats-offline]` — `init_proxy_components()` сознательно НЕ зовётся:
  `!ctx.m_nats_client` → быстрый выход (все error/span-ветки первой трети
  `poll_response`);
* `NatsPollService: zero timeout falls back to the config timeout`
  `[nats-live]` — `poll_response(..., 0, ...)` → ветка
  `timeout_ms <= 0` (30000), отвечающий responder (тот же паттерн `poll.ok`);
* `NatsPollService: reconnects a dropped client and still replies`
  `[nats-live]` — `disconnect()` перед poll → `poll_ensure_connected()`
  поднимает соединение заново (успешные reconnect-ветки 183/188–190).

### 3. `src/test_nats_live.cpp` — чистка nodiscard в pgtest-оснастке

Два `pg_redir(...)` внутри fork-детей (функция `[[nodiscard]]`) — 
игнорирование результата роняло `-Wunused-result` (шум round 74);
исправлено на `(void)pg_redir(...)`.

### 4. `src/l2_worker.hpp` — убрана vestigial-строка round 74

`friend class L2WorkerDbTestHook;` не использовался ни одним тестом (весь
доступ шёл через публичный `db_handler_ready()`) — удалён.

## Результаты (coverage-образ)

| Метрика | Было (baseline round 74c/80) | Стало |
| --- | --- | --- |
| Строки production | 6350/7112 = 89.29% | 6369/7113 = **89.54%** |
| Ветви production | 6937/13407 = 51.74% | 6953/13411 = **51.85%** |
| `l2_worker_nats.cpp` строки | 302/361 | **315/361** (+13, ветви 377→**391/826**) |
| `nats_poll_service.cpp` | 110/125 строк, 112/272 ветви | **115/125, 116/272** |
| Строки полный scope | — | 14936/15961 = 93.58% |

Coverage-стадия зелёная: 551 кейсов / 2537 ассерций (test_components) +
120/883 (test_proxy_core), регресс-гейт после `--update` — OK (допуск 0.0).

## Гейты

* `--fail-under-line 90` — OK (93.58%)
* `--fail-under-branch 40` полный scope — OK (41.13%)
* `--fail-under-branch 50` production-only — OK (51.85%)
* clang-tidy по изменённым файлам — без предупреждений
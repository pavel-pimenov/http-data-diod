# Round 115: покрытие success- и invalid-response путей `handle_request` живьём

Основной (generic) путь прокси `RequestHandler::handle_request` был покрыт лишь
до таймаута: все офлайн-тесты `test_proxy_handlers.cpp` вызывают `handle_get`/
`handle_post` без брокера, `poll_for_response()` возвращает пустоту, и запрос
завершается `504 Timeout waiting for response`. В результате **успешная
ветвь** (разбор ответа + `send_response`) и ветвь **невалидного ответа** не
исполнялись ни разу.

## Что было не покрыто

`request_handler.cpp`:

* `547` разбор ответа (`JsonUtils::try_parse(response_data_str)`);
* `550-552` success → `send_response(...)`;
* `447-460` тело `send_response` (лог «Request completed») — функция вообще не
  вызывалась;
* `554-559` `invalid_response` → `500 "Invalid response format"`.

При этом соседний `525-529` (`queue_failed`, «Failed to queue request»)
недостижим: `NatsPushService::push_request()` только собирает конверт и всегда
возвращает непустую строку — это защитный guard, оставлен как есть.

## Новый тест

`TEST_CASE("RequestHandler: generic backend 200 and invalid response over a live
broker", "[nats-live]")` в `src/test_nats_live.cpp` (рядом с DB-gateway лайв-тестом):

1. **success.** На NATS-субъект прокси (`NATS_SUBJECT`) подписан `NatsResponder`,
   который отвечает конвертом `build_nats_response_envelope(200, …, {"proxied":
   true}, …)`. Тогда `handle_get("/api/v1/anything")` (путь вне `/v1/sql`, то
   есть generic `handle_request`) проходит push → poll → **разбор** → ответ
   `200 {"proxied":true}`. Это первое живое исполнение `send_response`.
2. **invalid response.** Тот же путь, но responder отвечает `"not-a-json-
   envelope"` → `JsonUtils::try_parse` терпит неудачу → `554-559` → `500
   "Invalid response format"`.

Ответчик использует публичный контракт `build_nats_response_envelope`, поэтому
тест заодно страхует форму worker→proxy конверта от дрейфа.

## Побочный фикс: детерминированный async-error в `nats_client.cpp`

Первый прогон покрытия round-115 дал регрессию `nats_client.cpp` 91.99% →
89.83%: в этом прогоне **ни разу** не сработал асинхронный колбэк `error_cb` →
`on_async_nats_error` (строки `318-321`). Эти строки покрывались только как
побочный эффект таймингов в live-тестах переподключения воркера: по прогонам
событие «NATS error: asynchronous NATS error» встречалось 2 раза (round-113),
4 раза (round-114) и 0 раз (round-115). Baseline держался на флейки.

Прогон каждого `[nats-live]`-теста по отдельности показал, что единственный
источник этих событий — `L2Worker: reconnects and keeps serving after the broker
restarts` (ровно 1 событие, у остальных 0). Внутри libnats `error_cb` вызывается
не на разрыв связи как таковой, а только на конкретные асинхронные ошибки
(`_processPermissionViolation`, `_processAuthError`, slow consumer,
`_pushDrainErr`). Разрыв брокера напрямую эту ошибку не порождает, поэтому
«остановить брокер и подождать» — не детерминированный триггер.

Промежуточные попытки (обе отвергнуты как флейки и не попали в код):

* «остановить брокер и ждать async-error в `get_last_error()`» — 0 событий в
  прогоне, где их не случилось;
* «остановить брокер и вызвать `client.drain()`» — связь успевает перейти в
  CLOSED, `natsConnection_DrainTimeout` возвращает `Illegal State` и ошибку не
  публикует;
* «заморозить брокер `SIGSTOP` и слить» — зависит от того, успел ли брокер
  ответить на PING до остановки, и от перетирания записи последующей
  `mark_disconnected` (3 из 12 прогонов падали).

Детерминированный тест сделан через **нарушение прав доступа** — ошибку
генерирует сам сервер, связь при этом не рвётся и запись не перетирается:

`TEST_CASE("NatsClient: async error callback records a permission violation",
"[nats-live]")` — поднимается nats-server с конфигом (`authorization.users`), где
пользователю разрешён коннект, но запрещена публикация (`publish: { deny: ">" }`).
Клиент подключается под этим пользователем и публикует в запрещённый субъект;
сервер отвечает `-ERR 'Permissions Violation for Publish to ...'`, libnats
доставляет это в `error_cb` → `_processPermissionViolation` → `on_async_nats_error`
(`get_last_error()` становится `"asynchronous NATS error: Not Permitted"`), тест
ждёт этого через `wait_for_condition` (до 8 с). Подтверждено 12/12 прогонов, по
одному async-error на прогон.

Для этого `NatsServer` в тесте получил необязательный конфиг-файл
(`use_config_file()`, запуск `nats-server -c`), обычные тесты продолжают
поднимать брокера прежним способом.

## Почему это достижимо и детерминировано

`handle_request` уже вызывается офлайн-тестами 11 раз; не хватало только
живого ответчика на субъекте прокси. `NatsPollService` в режиме proxy выполняет
запрос к `m_ctx.m_config.m_nats.m_subject` (тот же `NATS_SUBJECT`), поэтому
`NatsResponder` на этом субъекте отвечает в reply-to инбокс — та же схема, что
в уже существующих `[nats-live]` round-trip-тестах `NatsPollService`.

## Итог

| метрика | было | стало |
|---|---|---|
| production строки | 6501/7139 = 91.06% | **6516/7139 = 91.27%** |
| production ветви | 7144/13493 = 52.95% | **7176/13493 = 53.18%** |

`request_handler.cpp`:

| метрика | было | стало |
|---|---|---|
| строки | 413/449 = 91.98% | **428/449 = 95.32%** |
| ветви | 499/961 | **526/961** |

`nats_client.cpp` вернулся на baseline 425/462 = 91.99% (всего по проекту:
15713/16708 = 94.04% строк, 30602/74488 = 41.08% ветвей). Baseline переанкорен.

## Проверка

* `./scripts/run-coverage.sh` — ✅ (гейты стадии: строки 94.04% ≥ 90,
  ветви 41.08% ≥ 40; production-ветви 53.18% ≥ 50; пер-файловая сверка —
  регрессий нет, production 6516/7139 = 91.27%).

  Замечание: стадия покрытия периодически (~20% прогонов, в изоляции 4/20)
  падает на **не связанном с раундом** флейке round-112
  `TraceLogger: jaeger delivery recovers after the endpoint comes back`
  (`test_trace_logger.cpp:451`): зарезервированный через `bind_to_any_port`
  порт иногда вместо `connection refused` даёт read-таймаут, и второй батч не
  доставляется. Это отдельный предмет, здесь только зафиксирован.
* `./rebuild-and-run.sh` — ✅ (сборка зелёная, 582 тест-кейса
  `test_components` + `test_proxy_core`, все сервисы healthy).
* `./health-check.sh all` — ✅; `message_counter.py --iterations 1
  --concurrent 1` — ✅ (сервис-путь доставки сообщений без потерь).
* `./scripts/run-cppcheck.sh` — ✅ (0 замечаний).
* `./scripts/run-clang-tidy.sh` — ✅ (0 замечаний).

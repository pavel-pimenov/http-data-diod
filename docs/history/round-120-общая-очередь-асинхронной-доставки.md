# Round 120: общая очередь асинхронной доставки (`SentryClient` ↔ `JaegerLogger`)

## Что сделано

Оба асинхронных отправителя — `SentryClient` (очередь ошибок) и
`JaegerLogger` (очередь спанов/транзакций) — несли один и тот же каркас
очереди, продублированный почти дословно:

* `std::deque<T>` + `std::mutex` + `std::condition_variable_any` под общим
  локом;
* `push`-путь с обработкой переполнения и `notify_one`;
* ожидание батча (у Sentry — бесконечное по `condition_variable_any::wait`,
  у Jaeger — с таймаутом `flush_interval/10`);
* выемка до `max_batch` элементов под локом;
* учёт «взято, но ещё не обработано» для `flush()` (pending) у Sentry;
* дренаж остатка на shutdown у Jaeger;
* последовательность остановки (`request_stop` → `notify_all` → `join`).

При этом семантика заметно различалась, поэтому вынесена только **очередь**
(хранилище + синхронизация + политики), а циклы доставки остались в классах:

* переполнение: Sentry дропает **старейший** (`DropOldest`), Jaeger —
  **входящий** (`DropNewest`);
* ожидание: Sentry — бесконечное, Jaeger — с idle-таймаутом;
* shutdown: Sentry досылает очередь в своём цикле (`drain_on_stop=true`),
  Jaeger немедленно выходит и досылает остаток fire-and-forget (`drain`);
* счётчики (`events_failed` / `spans_failed`) и гейджи остаются у владельца —
  очередь лишь возвращает исход `push` (`PushOutcome{m_accepted, m_dropped,
  m_size}`).

## Изменения

* **`src/async_sender_queue.hpp`** (новый) — шаблон `AsyncSenderQueue<Item>`:
  * `PushOutcome push(Item)` — переполнение по политике `OverflowPolicy`
    (`DropOldest`/`DropNewest`), `notify_one`;
  * `TakeResult wait_and_take(st, idle_timeout_ms, max_batch, drain_on_stop,
    out)` — `Batch`/`Idle`/`Stopped`; учитывает обе shutdown-семантики;
  * `mark_processed(n)` + `flush()` — учёт «взято, но не обработано»;
  * `drain(out)` — выемка остатка на shutdown;
  * `notify_all()` — разбудить потребителя при остановке;
  * `size()`.
* **`sentry_client.hpp/.cpp`** — `QueueState` заменён на
  `AsyncSenderQueue<sentry::SentryEvent> m_queue` + отдельный
  `std::jthread m_sender_thread`; `capture`/`flush`/`sender_loop`/деструктор
  переведены на методы очереди, `m_max_queue_size` передаётся в конструктор
  очереди, политика `DropOldest`, батч 8, `drain_on_stop=true`.
* **`trace_logger.hpp/.cpp`** — `Delivery::SpanQueue` заменён на
  `AsyncSenderQueue<SpanData>` (инициализация `g_tracing_max_queue_size`,
  политика `DropNewest`); `enqueue_span`/`sender_loop`/деструктор переведены на
  очередь, idle-таймаут `flush_interval/10`, `drain_on_stop=false`, остаток на
  shutdown по-прежнему досылается fire-and-forget (`send_batch`) с отдельными
  счётчиками и `deliver_sentry_transactions`.
* **`src/test_async_sender_queue.cpp`** (новый) — 7 прямых юнит-тестов очереди:
  FIFO-порядок батча, обе политики переполнения, `Idle` по таймауту, обе
  семантики `Stopped` (`drain_on_stop` true/false), удержание `flush` до
  `mark_processed`, `drain`.
* **`src/CMakeLists.txt`** — новый тестовый TU в `test_components`.

Поведение не менялось: тексты логов, метрики, форматы и тайминги сохранены
один-в-один; публичный API `SentryClient`/`JaegerLogger` не тронут.

## Проверка

* `./rebuild-and-run.sh`: rc=0, юнит-тесты `All tests passed (2816 assertions in 589 test cases)`
  (+45 assertions / +7 cases — новый `test_async_sender_queue.cpp`).
* `./health-check.sh all`: rc=0.
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0 (без потерь/перекрёстных ответов).
* clang-tidy по изменённым файлам — 0 предупреждений (заодно закрыты
  `misc-include-cleaner`, `performance-enum-size`, `performance-unnecessary-value-param`,
  `misc-const-correctness`).
* cppcheck по изменённым файлам — чисто.
* coverage: новый `async_sender_queue.hpp` — 100% строк (65/65); общий
  production — 91.39% (6541/7157). Baseline переанкорен
  (`scripts/coverage-regression-check.py --update`): добавлен новый заголовок
  и учтён накопленный с round 115 дрейф (в т.ч. правки раундов 116–119, не
  фиксировавшиеся в baseline). Сдвиги `sentry_client.cpp` (98.35%→98.28%) и
  `trace_logger.cpp` (94.12%→93.65%) объясняются переносом полностью покрытых
  строк очереди в общий заголовок; `l2_worker.cpp`/`proxy_init.cpp` не
  менялись — 2-строчный дрейф от тайминга тестов.
* `scripts/metrics-consistency-check.py --offline` — ок (метрики не менялись).

Существующие тесты `[sentry-client]` (переполнение с дропом старейшего, `flush`)
и `[tracing]` (полная очередь с дропом входящего, idle-таймаут, shutdown-дренаж,
предохранитель) покрывают обе ветви-потребители без изменения кода тестов.

# round 150 — clang-tidy: обращение к статическому члену через объект (+ рефакторинг `crash_handler.hpp`)

## Что сделано

Включена третья из «статических» проверок:

```
readability-static-accessed-through-instance,
```

Полный свип дал **49 находок** в проектных файлах: 30 в `crash_handler.hpp`,
5 в `tracing_helpers.hpp`, 5 в `common_utils.cpp`, по 1–3 в остальных.

### `crash_handler.hpp`: рефакторинг вместо механических правок

Вложенные структуры `CrashFrames` и `SentryDsnConfig` — «namespace'ы в
структуре»: все их поля объявлены `static inline`, а обращались к ним как
`m_frames.m_addrs[i]`, `m_sentry.m_host` (сами объекты `m_frames`/`m_sentry`
при этом пустые). Проверка видит здесь доступ к статическому члену через
объект, и её автофикс разворачивал каждое из 30 мест в полное имя:

```cpp
    CrashHandler::SentryDsnConfig::m_host = host_port.substr(0, colon);
```

Это чинит симптом, но делает код заметно хуже читаемым. Пробовались два
варианта:

1. **Сделать поля обычными членами, а статическим оставить один
   объект-инстанс.** Так `m_sentry.m_host` — уже обычный доступ к полю через
   статический объект. Но первая же сборка показала, что «просто убрать
   `static` из полей» не собирается: инициализатор-член (`m_count = 0`,
   `m_port = 0`, `m_addrs{}`) **вложенного** класса недоступен там, где
   объявлен статический экземпляр внутри *объемлющего* класса — он ещё не
   доопределён:

   ```
   crash_handler.hpp:72:10: error: default member initializer for 'm_port' needed
     within definition of enclosing class 'CrashHandler' outside of member functions
   ```

   Обходной путь (явные конструкторы у структур) тоже оказался с подвохом:
   `modernize-use-default-member-init` тут же просит вернуть инициализаторы
   обратно в инициализаторы-члены — то есть ровно то, что запрещено языком.
   Складывать один обход на другой и вешать NOLINT'ы смысла нет.

2. **Оставить поля статическими, выкинуть пустые объекты `m_frames`/`m_sentry`
   и обращаться к полям через имя самой структуры**: `SentryDsnConfig::m_host`,
   `CrashFrames::m_addrs[i]`, `CrashFrames::m_count`. Структуры честно
   объявляются тем, чем и были — namespace-подобным хранилищем, а обращение
   через «объект, которого нет» исчезает.

Выбран второй: одна механическая правка, никаких обходов правил языка, ни
одного NOLINT'а, ни одной отключённой проверки. В комментарии рядом со
структурой отмечено, почему члены статические (и почему инстанс тут не
годится). Семантика не изменилась: и раньше был ровно один набор полей, а
сигнальный хендлер по-прежнему работает со статическими данными
(async-signal-safe путь не затронут: ни аллокаций, ни новых вызовов).
Единственная константа осталась `static constexpr int kMaxFrames`, а её
единственное использование переписано на `CrashFrames::kMaxFrames` (в
комментарии выше тоже).

### Остальное: настоящие «статические методы через объект»

Это вызовы статических методов через указатель/объект-экземпляр, то есть ровно
то, что проверка и должна ловить:

- `JaegerLogger::parse_traceparent` / `generate_trace_id` / `generate_span_id` /
  `generate_traceparent` — в `common_utils.cpp` (5 мест, стало статическими в
  round 149, поэтому проверка «проявилась» именно сейчас), `tracing_helpers.hpp`
  (5 мест), `l2_worker.cpp`, `nats_push_service.cpp`, `request_handler.cpp`
  (2 места) и `test_trace_logger.cpp` (4 места).
- `InFlightTracker::shard_index` — `in_flight_tracker.hpp`.
- `L2Worker::extract_l2_server_span_id` / `prepare_response_headers` /
  `prepare_response_data` — `test_l2_worker.cpp`.

10 мест закрыл автофикс clang-tidy (`tracer->generate_span_id()` →
`JaegerLogger::generate_span_id()`), остальные 7 пришлось поправить руками:
для `m_ctx.m_tracer->…` и тестовых `env.m_logger->…` clang-tidy фикс не
предложил (выражение-объект сложнее простого указателя).

**Что важно:** guard-условия `if (m_ctx.m_tracer)` оставлены как есть. После
правки они выглядят «декоративными» (статический метод не нуждается в
указателе), но указатель здесь — флаг «трассировка настроена»: тесты прямо
проверяют, что без трассера traceparent в envelope не появляется
(`NatsPushService: serializes the backend request`: «The tracer only exists in
main.cpp, so no traceparent can be generated»), а `add_proxy_trace_fields`
принимает сам указатель. Убирать проверку — значит менять поведение.

### Побочная чистка

`L2WorkerTestAccess` — три аксессора принимали `L2Worker &worker`, который
после round 149 стал не нужен (методы статические): параметр убран, пять мест
вызова обновлены (`extract_l2_server_span_id(worker, post.m_body)` →
`extract_l2_server_span_id(post.m_body)`). Плюс вёрстка: выравнивание
продолжения в `common_utils.cpp`, перенос строки в `in_flight_tracker.hpp`,
комментарий в `crash_handler.hpp`.

## Проверка

- `./rebuild-and-run.sh` — rc=0 (`All tests passed (2814 assertions in
  589 test cases)`).
- clang-tidy `--all` (47 TU) — ноль предупреждений в проектных файлах.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck (`./scripts/run-cppcheck.sh`) — чисто.
- coverage (`./scripts/run-coverage.sh`) — rc=0: `coverage unit tests: 106s,
  all passed`, production 6526/7139 = 91.41%, `OK: регрессий покрытия нет`.
  Первый прогон coverage упал не на покрытии, а на живом NATS-тесте из
  `test_components` («NatsClient: pub/sub round-trip, headers and queue
  subscribers», `REQUIRE(queue_a.load() > 0)`): под инструментацией сабскрайбер
  очереди не успел получить сообщение. Обычная сборка тот же тест прошла, а
  повторный прогон coverage — зелёный; это известный тайминговый флейк, не
  связанный с правками раунда (в них нет ни одного изменения в NATS-клиенте
  или его тестах, кроме `test_nats_live.cpp`, где тронут только `wait_ready`).

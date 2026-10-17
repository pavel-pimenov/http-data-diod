# round 135 — общие сетевые/ожидающие хелперы live-тестов

## Что сделано

Два live-тестовых TU (`test_db_live.cpp` — реальный postgres, `test_nats_live.cpp`
— реальный nats-server) держали по своей копии одного и того же каркаса, а
внутри `test_nats_live.cpp` тот же каркас был продублирован ещё раз в
компактном `namespace pgtest`:

- резерв эфемерного loopback-порта — **3 копии** (`test_db_live.cpp`,
  анонимный namespace `test_nats_live.cpp`, `pgtest::pg_reserve_port`);
- ожидание условия с дедлайном — **3 копии** (`wait_for_condition` в обоих TU,
  с `while`- и `do/while`-вариантами, + `pgtest::pg_wait`).

Оба хелпера вынесены в общий тестовый заголовок `src/test_support.hpp` (он
назван с префиксом `test_` и потому не попадает в coverage-гейт):

```cpp
// Reserve an ephemeral loopback TCP port: bind to port 0, read the port the
// kernel picked, release it.
[[nodiscard]] inline int reserve_loopback_port();

// Poll `predicate` every 50 ms until it returns true or `timeout_ms` elapses;
// the predicate is checked once more after the deadline ...
[[nodiscard]] inline bool wait_for_condition(
    const std::function<bool()> &predicate, int timeout_ms);
```

Реализация `wait_for_condition` взята у `while`-варианта (он же был в
`test_db_live.cpp` и `pgtest`): разница с `do/while`-вариантом — только проверка
дедлайна до первого вызова предиката, при живых таймаутах 5–15 c поведение
идентично (предикат всё равно вызывается сразу, потому что дедлайн ещё не
истёк; в конце — тот же `return predicate()`).

В TU удалены все шесть определений, на их месте — точечные
`using test_support::reserve_loopback_port;` / `using test_support::wait_for_condition;`
(в `test_nats_live.cpp` — и в анонимном namespace, и в `pgtest`), два вызова
`pgtest` обновлены на новые имена. Попутно:

- в `test_support.hpp` добавлены прямые `<chrono>`, `<functional>`,
  `<netinet/in.h>`, `<sys/socket.h>`, `<thread>`, `<unistd.h>`;
- из `test_db_live.cpp` убраны ставшие ненужными `<netinet/in.h>` и
  `<sys/socket.h>` (единственным их потребителем был удалённый порт-хелпер).

Итог по диффу: −42 строки (−102/+60), дублирование каркаса устранено полностью.

## Проверка

- `./rebuild-and-run.sh` — rc=0, все юнит-тесты прошли.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто (в первой итерации всплыл `misc-include-cleaner` на
  `<arpa/inet.h>` в `test_support.hpp` — лишний, убран).
- coverage (`./scripts/run-coverage.sh`) — rc=0, `OK: регрессий покрытия нет`.
  Изменения только в тестовых TU (префикс `test_` исключён из per-file гейта),
  поэтому production-цифры не сдвинулись: 6523/7137 = 91.40%; baseline не
  трогался.

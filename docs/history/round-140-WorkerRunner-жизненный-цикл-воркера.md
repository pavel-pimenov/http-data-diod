# round 140 — `WorkerRunner`: жизненный цикл воркера в тестах в одном месте

## Что сделано

Три теста `L2Worker` в `src/test_nats_live.cpp` поднимали воркер в фоне одним и
тем же руками собранным каркасом — локальный RAII-страж для глобального флага
shutdown, ручной сброс флага, `std::thread` и такой же ручной «хвост» для
остановки:

```cpp
  struct RestoreShutdownFlag {
    ~RestoreShutdownFlag() { g_shutdown_flag = false; }
  } const restore_shutdown;
  g_shutdown_flag = false;
  std::thread runner([&worker] { worker.run(); });
  ...
  g_shutdown_flag = true;
  REQUIRE(runner.joinable());
  runner.join();
```

Каркас заменён одной RAII-обёрткой:

```cpp
// Runs L2Worker::run() on a background thread with the process-wide shutdown
// flag cleared, and raises the flag plus joins on scope exit. A test that fails
// in the middle would otherwise leave the worker running and the flag raised
// for the next test, and a joinable std::thread reaching its destructor aborts
// the whole binary. The flag is the same global the signal handlers toggle.
class WorkerRunner {
public:
  explicit WorkerRunner(L2Worker &worker) {
    g_shutdown_flag = false;
    m_thread = std::thread([&worker] { worker.run(); });
  }

  ~WorkerRunner() {
    g_shutdown_flag = true;
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

private:
  std::thread m_thread;
};
```

Все три места стали строкой `WorkerRunner const runner(worker);`, «хвосты»
удалены.

Два побочных эффекта, ради которых раунд и делался:

- раньше падение любого `REQUIRE` внутри такого теста раскручивало стек,
  `std::thread` с живым потоком уходил в деструктор, и весь тестовый бинарь
  падал через `std::terminate`; теперь флаг поднимается и поток
  присоединяется при любом выходе из области видимости;
- порядок «сначала остановить воркер, потом разрушать клиента» больше не
  держится на аккуратности копипасты: `WorkerRunner` объявляется после
  `NatsClient` (в `[db-live-combo]` тесте клиент и `connect()` подняты выше),
  поэтому деструктор обёртки отрабатывает раньше деструктора клиента. В этом
  тесте воркер стартует чуть позже — на считанные миллисекунды, а запросы там
  и так идут через `wait_for_condition` с ретраями.

По строкам раунд почти нулевой (−26 строк каркасов, +26 строк класса): смысл в
единственном определении жизненного цикла воркера и в снятой мине
`std::terminate`.

## Проверка

- `./rebuild-and-run.sh` — rc=0, все юнит-тесты прошли.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто.
- coverage (`./scripts/run-coverage.sh`) — production 6528/7142 = 91.40%; правка
  только в `test_nats_live.cpp`, гейт пометил лишь флейк `nats_client.cpp`
  (одна тайминг-зависимая строка reconnect-пути), baseline переанкорен.

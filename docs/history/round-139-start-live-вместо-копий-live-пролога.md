# round 139 — `NatsServer::start_live()` вместо 23 копий live-пролога

## Что сделано

`src/test_nats_live.cpp` — единственный файл, который поднимает настоящий
nats-server (`NatsServer` форкает бинарь на зарезервированном порту). Каждый
live-тест начинался с одного и того же пролога, **23 копии**:

```cpp
  if (nats_binary_path().empty()) {
    SKIP("nats-server binary not available");
  }
  NatsServer server;
  REQUIRE(server.start());
  REQUIRE(server.wait_ready());
```

Пролог переехал в метод самого сервера (рядом с уже существующим `restart()`,
который так же пакует `start()` + `wait_ready()`):

```cpp
  // Starts the broker for a live test, or marks the test skipped when the
  // nats-server binary is not part of the image (SKIP throws, so it unwinds
  // through the caller like the guard it replaced). The guard and the two
  // start assertions used to be copy-pasted at the top of every [nats-live]
  // test and re-typed around each extra broker inside a test.
  void start_live() {
    if (nats_binary_path().empty()) {
      SKIP("nats-server binary not available");
    }
    REQUIRE(start());
    REQUIRE(wait_ready());
  }
```

22 из 23 мест стали строкой `server.start_live();` (три теста используют
однострочный вариант `NatsServer s; REQUIRE(s.start()); REQUIRE(s.wait_ready());`
— тоже сведены к `s.start_live();`). Отдельно закрыты два нестандартных места:
тест на permissions-violation (`use_config_file(config_path)` + start) и
`[db-live-combo]` (nats-пролог стоял перед проверкой Postgres).

`SKIP("nats-server binary not available")` осталось в двух местах: в самом
методе и в тесте `NatsClient: connect to a down broker succeeds once it starts`,
где страж обязан сработать **до** запуска брокера — к моменту позднего
`REQUIRE(server.start())` в тесте уже живёт `std::thread runner`, а `SKIP`
бросает исключение, и раскрутка стека на joinable-потоке дала бы
`std::terminate`. Там пролог оставлен как был.

### Проверка семантики `SKIP` в хелпере

Перенос `SKIP` внутрь функции требует, чтобы Catch2 действительно раскручивал
стек. Это проверено отдельной пробой в builder-образе (`/tmp/skip_probe.cpp`):
`SKIP` из вспомогательной функции помечает тест skipped, а тело после вызова
хелпера не исполняется (`test cases: 2 | 1 passed | 1 skipped`); в самом Catch2
для этого есть `[[noreturn]] throw_test_skip_exception()` и флаг
`AssertionHandler::shouldSkip`. В самом образе бинарь nats-server есть, поэтому
SKIP-ветка юнит-тестами не гоняется — пробу компилировал руками.

## Проверка

- `./rebuild-and-run.sh` — rc=0, все юнит-тесты прошли.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто.
- coverage (`./scripts/run-coverage.sh`) — production 6529/7142 = 91.42%; правка
  только в `test_nats_live.cpp`, поэтому гейт пометил лишь известные флейки
  (`metrics_history.hpp` 90 -> 87, `nats_client.cpp` 422 -> 423 в покрытых
  строках), baseline переанкорен.

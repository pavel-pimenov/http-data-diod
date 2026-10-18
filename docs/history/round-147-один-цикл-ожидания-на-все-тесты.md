# round 147 — один цикл ожидания на все тесты: `test_wait_support.hpp`

## Что сделано

В тестах нашлись **четыре независимые реализации одного и того же цикла**
«опросить предикат до дедлайна, потом проверить ещё раз» плюс два места, где
цикл был выписан прямо в теле теста:

| где | что было | гранулярность |
|---|---|---|
| `test_support.hpp` | `wait_for_condition(const std::function<bool()>&, int)` | 50 мс |
| `test_trace_logger.cpp` | локальная `wait_for_condition(const auto &, int)` | 25 мс |
| `test_coverage_ext.cpp` | `wait_for_tasks(const std::atomic<int>&, expected, timeout)` | 1 мс |
| `test_coverage_ext.cpp` | `wait_for_flag(std::mutex&, bool&, timeout)` | 1 мс |
| `test_components.cpp` | цикл `for (i < 40 && tracked_ips > 0) { sleep 100; cleanup(); }` в тесте TTL | 100 мс |
| `test_proxy_core.cpp` | цикл `for (i < 50 && !sampled) { sleep 100; sampled = …; }` в тесте `MetricsHistory` | 100 мс |

Цикл переехал в новый **dependency-light** заголовок
`src/test_wait_support.hpp`:

```cpp
template <typename Predicate>
[[nodiscard]] inline bool wait_for_condition(
    const Predicate &predicate, int timeout_ms,
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds(50));
```

Гранулярность стала явным параметром (а не спрятанной в копии константой):
`test_coverage_ext.cpp` просит 1 мс, остальные довольствуются дефолтом.
Третья копия шаблона ушла вместе с `std::function` — предикат теперь
подставляется, лишней аллокации на каждый вызов нет.

Отдельный заголовок, а не `test_support.hpp`, по той же причине, что и
`test_metrics_support.hpp` из round 143: `test_support.hpp` тянет
`app_context`/`request_handler`/`stats_logger`, а `test_trace_logger.cpp`,
`test_coverage_ext.cpp` и `test_proxy_core.cpp` — dependency-light TU. Ни один
из них не платит за тяжёлые включения.

Как сделано в каждом месте:

- `test_support.hpp` — прежнее имя сохранено ре-экспортом
  (`using test_wait_support::wait_for_condition;`), поэтому 8 вызовов в
  live-тестах (`test_db_live.cpp`, `test_nats_live.cpp`) не изменились;
  из заголовка убраны ставшие лишними `<chrono>`, `<functional>`, `<thread>`.
- `test_trace_logger.cpp` — локальная копия удалена, подключён общий заголовок
  и `using test_wait_support::wait_for_condition;`; 19 мест вызова не тронуты
  (25 → 50 мс опроса при бюджетах 2–20 с).
- `test_coverage_ext.cpp` — `wait_for_tasks` и `wait_for_flag` сохранены как
  именованные адаптеры (у них разные типы входа и 1 мс-шаг), но их тела больше
  не дублируют цикл.
- `test_components.cpp` — цикл заменён на
  `REQUIRE(test_support::wait_for_condition(..., 4000, 100ms))`, ручной
  `cleanup_expired_ips()` переехал внутрь предиката (проверок стало на одну
  больше: 2813 → 2814 assertions, тестов 589 как было).
- `test_proxy_core.cpp` — цикл заменён на
  `const bool sampled = test_wait_support::wait_for_condition(..., 5000)`.

Попутно: `std::lock_guard` в новом предикате `wait_for_flag` clang-tidy завернул
в `std::scoped_lock` (`modernize-use-scoped-lock`) — исправлено до зелёного.

## Проверка

- `./rebuild-and-run.sh` — rc=0, `All tests passed (2814 assertions in
  589 test cases)`.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck (`./scripts/run-cppcheck.sh`) — чисто.
- clang-tidy (изменённые файлы, включая новый заголовок) — чисто.
- coverage (`./scripts/run-coverage.sh`) — rc=0, `OK: регрессий покрытия нет`,
  production 6529/7143 = 91.40% (правка только в тестах).
  Первый прогон упал на **не связанном с раундом тайминг-флейке**
  `TraceLogger: jaeger delivery recovers after the endpoint comes back`
  (`test_trace_logger.cpp:390`, бюджет 20 с не хватил под инструментацией;
  поведение самого теста раунд не менял — только шаг опроса 25 → 50 мс, то есть
  до 400 проверок на 20 с). Повторный прогон: `coverage unit tests: 109s, all
  passed`, флейк не воспроизвёлся.

# round 137 — общая фикстура `test_support::ProxyHandler`

## Что сделано

Каждый тест, дергающий `RequestHandler`, начинался с одного и того же
трёхстрочного каркаса — **19 копий** (`test_proxy_handlers.cpp` — 12,
`test_nats_live.cpp` — 7):

```cpp
// было — 19 раз
std::atomic<bool> shutdown{false};
StatsLogger stats(ctx, shutdown);
RequestHandler handler(ctx, stats);
```

Три объекта связаны ссылками (логгер держит `shutdown`, хендлер держит
логгер) и обязаны жить вместе до конца теста, причём в правильном порядке
объявления/уничтожения — то, что копия за копией воспроизводилось вручную.
Каркас вынесен в `src/test_support.hpp`:

```cpp
// The three objects every RequestHandler-driven test needs together: the
// shutdown flag, the stats logger that borrows it and the handler that borrows
// the logger. Declaration order is construction order and the handler is
// destroyed first, so no reference outlives its target. The handle_* forwarders
// let a test body keep calling them as if it held a RequestHandler directly.
class ProxyHandler {
public:
  explicit ProxyHandler(AppContext &ctx)
      : m_stats(ctx, m_shutdown), m_handler(ctx, m_stats) {}

  void handle_get(const httplib::Request &req, httplib::Response &res) {
    m_handler.handle_get(req, res);
  }

  void handle_post(const httplib::Request &req, httplib::Response &res) {
    m_handler.handle_post(req, res);
  }

private:
  std::atomic<bool> m_shutdown{false};
  StatsLogger m_stats;
  RequestHandler m_handler;
};
```

Все 19 мест стали одной строкой `ProxyHandler handler(ctx);` (плюс
`using test_support::ProxyHandler;` в обоих TU), остальные строки тестов не
менялись — форвардеры `handle_get`/`handle_post` оставляют вызовы
`handler.handle_get(req, res)` как были.

`RequestHandler::handle_request(...)` в фикстуру не пробрасывается: это
приватный метод (публичный API — `handle_get`/`handle_post`), а трёхаргументные
`handler.handle_request(json, status, body)` в тестах относятся к другому
классу (DB-обработчик запросов). Это сразу поймал clang-tidy
(`'handle_request' is a private member`) до сборки — форвардер убран.

Попутно из `test_nats_live.cpp` убран ставший ненужным
`#include "stats_logger.hpp"` (предупреждение `misc-include-cleaner`).

## Проверка

- `./rebuild-and-run.sh` — rc=0, все юнит-тесты прошли.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто (после снятия приватного форвардера и лишнего include).
- coverage (`./scripts/run-coverage.sh`) — rc=0, `OK: регрессий покрытия нет`;
  правки только в `test_`-файле, production 6523/7137 = 91.40% без изменений,
  baseline не трогался.

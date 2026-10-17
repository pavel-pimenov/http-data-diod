# round 141 — учёт ответов proxy/server в одном хелпере

## Что сделано

Два HTTP-сервера в `src/main.cpp` считали ответы в семейство
`l2_*_responses_total` собственными лямбдами, и тело у них было одно и то же —
с точностью до того, какие пути пропускать и что у proxy дополнительно есть
счётчик in-flight:

```cpp
// было: proxy
        app_ctx.m_proxy.m_metrics->m_in_flight_requests.Decrement();
        const auto &path = req.path;
        if (path == "/metrics" || path.rfind("/health", 0) == 0 ||
            path.rfind("/debug", 0) == 0) {
          return;
        }
        app_ctx.m_proxy.m_metrics->m_responses_total
            .Add({{"status", std::to_string(res.status)}})
            .Increment();

// было: server
        const auto &path = req.path;
        if (path == "/metrics" || path.rfind("/health", 0) == 0) {
          return;
        }
        app_ctx.m_server.m_metrics->m_responses_total
            .Add({{"status", std::to_string(res.status)}})
            .Increment();
```

Общая часть вынесена в один хелпер (у l2-server'а `/debug`-маршрутов нет,
поэтому у него флаг выключен):

```cpp
// Response accounting shared by the proxy and server HTTP servers: every
// answered request except the observability endpoints is counted into the
// per-status family. The proxy additionally skips its /debug routes.
void count_response_status(const httplib::Request &req,
                           prometheus::Family<prometheus::Counter> &responses,
                           int status, bool skip_debug) {
  const auto &path = req.path;
  if (path == "/metrics" || path.rfind("/health", 0) == 0 ||
      (skip_debug && path.rfind("/debug", 0) == 0)) {
    return;
  }
  responses.Add({{"status", std::to_string(status)}}).Increment();
}
```

Обе лямбды стали однострочными вызовами, поведение сохранено (набор
пропускаемых префиксов и момент уменьшения in-flight счётчика у proxy не
менялись). `main.cpp` попутно получил явный `#include "prometheus/family.h"`
(его потребовал `misc-include-cleaner` после появления имени
`prometheus::Family`).

## Проверка

- `./rebuild-and-run.sh` — rc=0, все юнит-тесты прошли.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто.
- coverage (`./scripts/run-coverage.sh`) — rc=0, `OK: регрессий покрытия нет`;
  production 6528/7142 = 91.40% без изменений (`main.cpp` в замер юнит-покрытия
  не попадает).

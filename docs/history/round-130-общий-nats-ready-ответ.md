# round 130 — общий NATS-ready ответ (`set_health_ready_nats`)

## Что сделано

Тело ответа `/health/ready` для NATS-сервисов было дословно продублировано в
двух точках (worker и proxy), отличаясь только именем сервиса:

```cpp
// main.cpp (l2-worker) / request_handler.cpp (l2-proxy)
res.status = 200;
res.set_content(
    R"({"status": "ready", "service": "l2-worker", "messaging": "nats"})",
    "application/json");
```

Шаблон вынесен в `json_response_utils.hpp` — рядом с уже существующими
`set_health_alive`/`set_health_ready`:

```cpp
inline void set_health_ready_nats(httplib::Response &res,
                                  std::string_view service) {
  res.status = 200;
  res.set_content(
      std::format(
          R"({{"status": "ready", "service": "{}", "messaging": "nats"}})",
          service),
      "application/json");
}
```

Обе точки вызова сведены к одной строке (`set_health_ready_nats(res,
"l2-worker")` / `"l2-proxy"`); сопутствующая установка метрик
(`m_nats_connected`, `m_health_ready`) осталась на местах. Вывод
байт-в-байт совпадает с прежними литералами (та же раскладка полей), так что
тесты, разбирающие JSON и проверяющие `status == "ready"`, отвечают одинаково.
Отдельный хелпер (а не третий параметр у `set_health_ready`) выбран, чтобы не
менять формат уже используемого l2-server'ом варианта без `messaging`.

## Попутно (предсуществующие предупреждения clang-tidy)

`main.cpp`/`request_handler.cpp` впервые попали в набор изменённых для
clang-tidy, из-за чего всплыли предупреждения в `metrics_manager.hpp`
(popадает туда через header-filter):

- `readability-identifier-naming` на function-local `static const
  std::vector<double> buckets` — переименован в `g_latency_buckets`
  (в `src/.clang-tidy` задан `StaticConstantPrefix: g_`);
- `misc-include-cleaner` на `std::size_t` и `uint64_t` — добавлены прямые
  `#include <cstddef>` и `#include <cstdint>`.

Поведение не менялось.

## Проверка

- `./rebuild-and-run.sh` — rc=0, **All tests passed (2816 assertions in 589
  test cases)**.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто (`✓ no errors or warnings in project files`).
- coverage (`./scripts/run-coverage.sh`) — после правки гейт показал `REGRESSION
  request_handler.cpp: 95.32% -> 95.29% (-0.03 п.п.)`. Это арифметика: тело
  ответа ушло из файла (4 покрытые строки) и заменилось одной строкой-вызовом
  хелпера, а непокрытых строк осталось столько же (21) — процент упал только
  потому, что файл стал короче. `json_response_utils.hpp`: 24/24 → 30/30 (новый
  `set_health_ready_nats` полностью покрыт proxy-тестами readiness). Baseline
  переанкорен (`coverage-regression-check.py --update`), повторная проверка —
  `OK: регрессий нет`, общий production 6519/7133 = 91.39%.

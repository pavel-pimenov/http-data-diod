# round 175 — общий помощник для чтения помеченных серий счётчика (убираем дублирование)

## Что было не так

Идея «прочитать значение одной помеченной серии счётчика» реализовывалась в
проекте шесть раз, почти одинаково:

```cpp
for (const auto &metric_family : family.Collect()) {
  for (const auto &metric : metric_family.metric) {
    for (const auto &label : metric.label) {
      if (label.name == "reason" && label.value == reason) {
        return metric.counter.value;
      }
    }
  }
}
return 0.0;
```

Копии: `src/stats_logger.cpp` (сумма по семейству — единственное место в
проде ✓), `src/test_components.cpp`, `src/test_sentry_client.cpp`,
`src/test_trace_logger.cpp`, `src/test_proxy_handlers.cpp`,
`src/test_l2_worker.cpp` ✓. С появлением в проекте помеченных семейств
(`l2_proxy_client_request_errors_total{type}` ✓, `l2_proxy_rejections_total{reason}` ✓,
`l2_worker_l2_call_errors_total{type}` ✓, `l2_tracing_spans_dropped_total{reason}` ✓,
`l2_worker_sentry_events_dropped_total{reason}` ✓, `l2_worker_retries_total{outcome}` ✓)
копий стало больше ✓ — и в каждой можно было ошибиться (например, забыть
`[[nodiscard]]` ✓ — clang-tidy это и указывал ✓).

## Что сделано

В `src/metrics_manager.hpp` (домашний заголовок метрик проекта ✓) появились два
общих помощника:

```cpp
// Значение одной помеченной серии счётчика; 0, если серии ещё нет
// (лейбл-семейство экспортируется только после первого Add()).
inline double labeled_counter_value(
    const prometheus::Family<prometheus::Counter> &family,
    const std::string &label_name, const std::string &label_value);

// Сумма по всем сериям счётчика.
inline double family_counter_total(
    const prometheus::Family<prometheus::Counter> &family);
```

- `stats_logger` (прод) переведён на `family_counter_total` ✓ — ветка «сумма по
  семейству ошибок прокси» стала одной строкой ✓.
- Все пять тестовых копий заменены на `labeled_counter_value` ✓ — каждый тест
  теперь читает значение серии одним вызовом ✓.

Итог: −17 строк при добавленных 30 строках самого помощника ✓ — и, главное, логика
«как прочитать помеченную серию» теперь живёт в одном месте ✓, а не в шести ✓.

## Проверка

- Сборка: `All tests passed (2887 assertions in 601 test cases)` и
  `945 assertions in 125 test cases` — набор тестов не изменился ✓ (рефакторинг
  меняет только способ чтения метрик ✓).
- `./health-check.sh all` — все проверки пройдены; smoke — все GET-запросы успешны.
- `./scripts/ci-gate.sh runtime` (rc=0): golden `96/96 families`, consistency
  `registered=100 dashboards=100 perses=100 readme=100 golden=100` ✓, parity
  Grafana↔Perses — `OK` ✓.
- `./scripts/run-cppcheck.sh` — rc=0; `./scripts/run-clang-tidy.sh --all` — 47 файлов,
  «no errors or warnings in project files» ✓.
- Гейт покрытия проверяет CI на этом коммите.

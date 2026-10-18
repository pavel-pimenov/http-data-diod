# round 143 — `test_support::make_counter/gauge/histogram`: регистрация метрик в тестах одной строкой

## Что сделано

Тестовые фикстуры и точечные тесты регистрировали метрики четырёх-пятистрочной
цепочкой prometheus-cpp, в которой от теста к тесту менялось только имя:

```cpp
// было — 51 место в четырёх TU
const httplib::Request req = ...;  // (пример из фикстуры)
m_spans_sent(prometheus::BuildCounter()
                 .Name("l2_tracing_spans_sent_total")
                 .Help("h")
                 .Register(m_registry)
                 .Add({})),
```

Цепочка свёрнута в один вызов — новый заголовок
`src/test_metrics_support.hpp` с тремя инлайн-хелперами:

```cpp
[[nodiscard]] inline prometheus::Counter &make_counter(
    prometheus::Registry &registry, const std::string &name,
    const std::string &help) {
  return prometheus::BuildCounter()
      .Name(name)
      .Help(help)
      .Register(registry)
      .Add({});
}

// `buckets` mirrors the second argument of Family<Histogram>::Add: the default
// empty vector means "library default buckets", exactly like .Add({}) did.
[[nodiscard]] inline prometheus::Histogram &make_histogram(
    prometheus::Registry &registry, const std::string &name,
    const std::string &help, const std::vector<double> &buckets = {}) {
  return prometheus::BuildHistogram()
      .Name(name)
      .Help(help)
      .Register(registry)
      .Add({}, buckets);
}
```

Всё стало строкой (в примере — с переносом по 80 колонкам):

```cpp
m_spans_sent(test_support::make_counter(
    m_registry, "l2_tracing_spans_sent_total", "h")),
```

Хелперы живут в отдельном заголовке, а не в `test_support.hpp`, намеренно:
`test_support.hpp` тянет за собой `app_context.hpp`, `request_handler.hpp`,
`stats_logger.hpp` и `trace_logger.hpp`, а `test_coverage_ext.cpp` по замыслу
«dependency-light (no NATS / DB)», и тащить в него контекст приложения ради
одного счётчика не хочется. Префикс `test_` в имени файла держит заголовок вне
гейта покрытия.

`help` оставлен явным параметром (а не дефолтом вроде `"test"`), чтобы справка
зарегистрированных семейств не менялась молча; `buckets` по умолчанию пустой
вектор — это ровно то, что делал `.Add({})`.

Переведено 45 мест в четырёх TU:
`test_coverage_ext.cpp` (20), `test_trace_logger.cpp` (20, включая шесть
гистограмм с явными бакетами `prometheus::Histogram::BucketBoundaries{...}` —
список бакетов нормализован в `std::vector<double>{...}`, это тот же тип),
`test_http_pipeline.cpp` (7), `test_sentry_client.cpp` (3).

Не тронуты (по построению хелперы возвращают одну метрику, а не family):

- цепочки, которые регистрируют family и добавляют серии позже
  (`.Register(*registry);` без `.Add`) — 6 мест в `test_coverage_ext.cpp` и
  `test_db_executor_base.cpp`;
- семейства с метками прямо в `.Add({{"ip", ...}})` — 4 места;
- `prometheus::BuildSummary()` с квантилями — 1 место (для summary хелпера
  нет, и не нужно).

Итог: −154 строки в тестах (254 удаления против 100 добавлений в четырёх TU
плюс 55 строк нового заголовка).

## Проверка

- `./rebuild-and-run.sh` — rc=0, все юнит-тесты прошли.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто (в том числе по новому заголовку: он линтится отдельно,
  `git add -N` нужен, иначе `git diff --name-only` его не видит).
- coverage (`./scripts/run-coverage.sh`) — rc=0, `OK: регрессий покрытия нет`;
  правка только в `test_`-файлах, production 6528/7142 = 91.40% без изменений.

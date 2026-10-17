# Round 122: `MetricsManager` — trait-билдер, дедуп `create_*` и чистка мёртвого параметра

## Что сделано

### 1. Дедупликация шести `create_*` через trait-билдер

Шесть обёрток над prometheus-cpp повторяли одну и ту же цепочку билдера:

* `create_counter` / `create_gauge` — `BuildX().Name().Help().Register(*r).Add({})`;
* `create_counter_family` / `create_gauge_family` / `create_histogram_family` —
  `BuildX().Name().Help().Register(*r)`;
* `create_histogram` отличается только `Add({}, buckets)`.

Введён `metrics_detail::Builder<Metric>` со специализациями для
`Counter`/`Gauge`/`Histogram` (`of()` → соответствующий `BuildX()`), и два
приватных шаблона `MetricsManager::create_single_metric<Metric>` /
`create_metric_family<Metric>`, в которых цепочка теперь записана **один раз**.
Публичные `create_*` остались прежними — тонкие строки-обёртки, вызывающие
шаблон (конструкция `Family` одинакова для всех типов, поэтому обобщается без
изменений). `create_histogram` оставлен отдельно — единственный, кому нужны
`buckets` на `Add`.

### 2. Удалён мёртвый параметр `buckets` у `create_histogram_family`

`create_histogram_family(registry, name, help, buckets)` принимал
`const std::vector<double> &buckets` и **никак его не использовал** — family не
помнит границы корзин, их передают на каждый `Family::Add(labels, buckets)`
(о чём уже говорил комментарий рядом). Параметр (и `std::array`-перегрузка)
убраны, 6 вызовов обновлены: 3 в `app_context.cpp`, 3 в
`test_coverage_ext.cpp` — все просто сбросили последний аргумент. Регистрация
метрик (имена/help) не менялась.

### 3. `latency_buckets_ms_to_10s()` больше не аллоцирует на каждый observe

Хелпер возвращал `std::vector<double>` **по значению**, а вызывается внутри
`observe_db_request_duration(...)`, то есть на каждый DB-запрос
(`request_handler.cpp`, `l2_worker_nats.cpp`) — 12 `double` в куче на запрос.
Теперь возвращается `const std::vector<double> &` на единственный
функционально-статический вектор (thread-safe инициализация), сама семантика
не меняется.

## Изменения

* **`src/metrics_manager.hpp`** — `metrics_detail::Builder<Metric>` (+3
  специализации), приватные `create_single_metric<Metric>` /
  `create_metric_family<Metric>`, у `create_histogram_family` убран `buckets`
  (и его `std::array`-перегрузка), `latency_buckets_ms_to_10s()` → `const&`.
* **`src/metrics_manager.cpp`** — тела `create_*` сведены к вызовам шаблонов;
  `create_histogram` остался прямым (нужны `buckets`).
* **`src/app_context.cpp`** — 3 вызова `create_histogram_family` без `buckets`.
* **`src/test_coverage_ext.cpp`** — 3 вызова `create_histogram_family` без
  `buckets`.

Публичный API (`create_*`, имена/help метрик) не менялся; per-`Add` корзины
передаются как прежде.

## Проверка

* `./rebuild-and-run.sh`: rc=0, юнит-тесты `All tests passed (2816 assertions in 589 test cases)`.
* `./health-check.sh all`: rc=0.
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0.
* clang-tidy / cppcheck по изменённым файлам — без новых замечаний (include-cleaner-шум предсуществующий).
* coverage: `metrics_manager.cpp` 24→16 строк (100%), `metrics_manager.hpp`
  18→25 строк (100%), общий production 91.39% (6542/7158); регрессий нет,
  baseline переанкорен (счётчики строк/ветвей обоих файлов сдвинулись при
  переносе логики в шаблоны).
* `scripts/metrics-consistency-check.py --offline` — ок (84/84; имена метрик
  не менялись).

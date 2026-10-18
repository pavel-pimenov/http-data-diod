# Round 92: clang-tidy `modernize-use-scoped-lock` — `std::lock_guard` → `std::scoped_lock`

## Что сделано

Закрыт пункт бэклэка round 90 `modernize-use-scoped-lock`: 31 конверсия
`std::lock_guard[<std::mutex>] lock(m)` → `std::scoped_lock lock(m)` в 6 файлах.
Правка чисто RAII-эквивалентная (один мьютекс — одинаковое поведение и время
жизни блокировки), покрытие не меняется.

Чекер был включён ещё в round 88 и тогда закрыл ~70 находок; оставшиеся 31
«прятались» тем же OOM-багом свипа, что нашёл round 90 (усечённый лог воркера,
убитого OOM-killer'ом).

Свип выполнен тем же детерминированным способом, что и round 91:

```sh
clang-tidy -p build-lint \
  --checks="-*,modernize-use-scoped-lock" \
  --header-filter="^/app/[^/]+\.(hpp|h)$" \
  --format-style=file --fix --fix-errors /app/*.cpp /app/*.cc
```

`--header-filter` оставляет вендорные заголовки нетронутыми, `--format-style=file`
форматирует только изменённые диапазоны, `--fix-errors` обходит мнимые
`consteval`-ошибки `fmt`/`spdlog`.

## Затронутые файлы (31 конверсия)

| файл | шт. | где |
| --- | --- | --- |
| `nats_client.cpp` | 11 | `m_connection.m_mutex` / `m_error_state.m_error_mutex` |
| `test_trace_logger.cpp` | 12 | хелперы ожидания и teardown `TEST_CASE` |
| `test_coverage_ext.cpp` | 3 | `wait_for_flag`, `TimedTaskQueue` |
| `dedup_cache.hpp` | 2 | `get` / `put` под `m_state.m_mutex` |
| `sentry_client.cpp` | 2 | `capture` и `sender_loop` (очередь) |
| `thread_pool.hpp` | 1 | `queue_size()` |

Проверка focused-прогоном по этим файлам: `no scoped-lock warnings`.
Остаются `std::lock_guard` в `dynamic_labeled_family.hpp:79,86` и
`test_coverage_ext.cpp:1665` (init-statement) — `clang-tidy` их **не** отмечает
(шаблонный контекст / init-statement), поэтому в рамках чекера не трогаются.

## Покрытие

Конверсии RAII-эквивалентны, новых/потерянных строк и ветвей быть не должно;
baseline `docs/coverage-baseline.json` не менялся. (Полный coverage-прогон в
round 91 показал, что production-файлы этой правки — `nats_client.cpp`,
`dedup_cache.hpp` — не теряют покрытие.)

## Проверка

* `./rebuild-and-run.sh`: rc=0; unit-тесты `all passed`
  (`=== unit tests (parallel): 98s, all passed ===`), сервисы подняты.
* `./health-check.sh all`: `All health checks passed!`
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0,
  `Success: All GET requests returned valid binary favicon data`.

Правки round 91 и round 92 сделаны в одном рабочем дереве (не коммитились
раздельно): файлы `nats_client.cpp` и `test_trace_logger.cpp` несут обе правки.

## Задел (следующие раунды)

Из бэклэка round 90 остаются: `misc-include-cleaner` в оставшихся файлах,
`misc-const-correctness` (383), `modernize-avoid-c-arrays` (64),
`modernize-raw-string-literal` (20), `modernize-use-nodiscard` (14),
`readability-identifier-naming` (7), `modernize-use-ranges` (4).

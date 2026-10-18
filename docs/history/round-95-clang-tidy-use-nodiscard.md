# Round 95: clang-tidy `modernize-use-nodiscard` — 10 `[[nodiscard]]`

## Что сделано

Закрыт пункт бэклэка round 90 `modernize-use-nodiscard`: 10 атрибутов
`[[nodiscard]]` добавлены на чистые const-геттеры/хелперы. В бэклоге «14» —
счёт завышен повторами в дублирующихся TU.

| файл | шт. | функции |
| --- | --- | --- |
| `stats_logger.hpp` | 1 | `ModeStats collect_mode_stats() const` |
| `test_db_executor_base.cpp` | 1 | `bool is_ready() const override` |
| `test_coverage_ext.cpp` | 4 | `is_ready() const override` (×2), `is_valid() const`, `get_last_status_code() const` |
| `test_nats_live.cpp` | 4 | `port() const`, `alive() const`, `run_wait(...)`, `spawn(...)` |

В проекте нет `-Werror` (проверено: в `CMakeLists.txt`/`Dockerfile` только
`nats/CMakeLists.txt` задаёт `-Wall -Wextra`, без `-Werror`), поэтому игнор
`nodiscard`-результатов даёт лишь warning и сборку не ломает — сборка зелёная.

## Свип

```sh
clang-tidy -p build-lint \
  --checks="-*,modernize-use-nodiscard" \
  --header-filter="^/app/[^/]+\.(hpp|h)$" \
  --format-style=file --fix --fix-errors /app/*.cpp /app/*.cc
```

Запуск от хост-пользователя (`--user $(id -u):$(id -g)`).

## Замечание о live-Postgres тестах

Первый прогон `./rebuild-and-run.sh` дал 4 падения в `test_db_live.cpp`
(`ex.init()`, `PQstatus(...) == CONNECTION_OK`, `handler.init({make_pg_cfg(shared_pg())})` ×2) —
встроенный Postgres в builder-стадии не поднялся; скрипт автоматически повторил
сборку без кэша, и повторный прогон прошёл: `All tests passed (2585 assertions
in 556 test cases)`. Это средовая флакона (те же тесты ранее были SKIPPED при
нехватке диска), не следствие правок round 95 (только `[[nodiscard]]`).

## Покрытие

`[[nodiscard]]` не меняет строки/ветви. `docs/coverage-baseline.json` не менялся.

## Проверка

* `./rebuild-and-run.sh`: rc=0 (после авто-ретрая);
  `test_components` — `All tests passed (2585 assertions in 556 test cases)`.
* `./health-check.sh all`: `All health checks passed!`
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0,
  `Success: All GET requests returned valid binary favicon data`.

## Задел

Остаются: `misc-include-cleaner` по оставшимся файлам, `modernize-avoid-c-arrays`
(64, без автофикса), `readability-identifier-naming` (7), `modernize-use-ranges` (4).

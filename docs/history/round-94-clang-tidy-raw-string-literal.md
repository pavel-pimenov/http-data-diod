# Round 94: clang-tidy `modernize-raw-string-literal` — 7 raw-строк

## Что сделано

Закрыт пункт бэклэка round 90 `modernize-raw-string-literal`. Уникальных мест
оказалось 7 (в бэклоге «20» — счёт завышен дублированием заголовка
`stats_page.hpp` по TU). Заменены строковые литералы с экранированными
кавычками на raw-литералы `R"(...)"`:

| файл | шт. | место |
| --- | --- | --- |
| `stats_page.hpp` | 5 | `build_sparkline_svg` (SVG, 3 фрагмента), `vrow` и `sparkwrap` в `build_stats_html` |
| `test_components.cpp` | 2 | `L2_SERVER_URLS` JSON-массив и `EnvVarGuard("L2_SERVER_URLS", …)` |

Пример (`stats_page.hpp:140`):

```cpp
// было
svg << "<svg class=\"spark\" viewBox=\"0 0 " << width << " " << height
    << "\" preserveAspectRatio=\"none\" width=\"100%\" height=\"" << height
    << "\"><polyline fill=\"none\" stroke=\"#79c0ff\" stroke-width=\"1.5\" points=\""
// стало
svg << R"(<svg class="spark" viewBox="0 0 )" << width << " " << height
    << R"(" preserveAspectRatio="none" width="100%" height=")" << height
    << R"("><polyline fill="none" stroke="#79c0ff" stroke-width="1.5" points=")"
```

Проверено: итоговый HTML/SVG байт-в-байт эквивалентен (raw-строка не меняет
содержимое, только убирает экранирование). Тест `message_counter` (fetches
`/favicon.ico` и stats-страницу) прошёл.

## Свип

```sh
clang-tidy -p build-lint \
  --checks="-*,modernize-raw-string-literal" \
  --header-filter="^/app/[^/]+\.(hpp|h)$" \
  --format-style=file --fix --fix-errors /app/*.cpp /app/*.cc
```

На этот раз clang-tidy запущен от хост-пользователя
(`docker run --user $(id -u):$(id -g) -e HOME=/tmp …`) — файлы остались
host-owned, побочного эффекта round 93 (root-owned) больше нет.

## Покрытие

Замена литералов не меняет число строк/ветвей. `docs/coverage-baseline.json` не
менялся. Полный coverage-гейт (live-Postgres) не перепроверялся — см. round 93.

## Проверка

* `./rebuild-and-run.sh`: rc=0; `=== unit tests (parallel): 96s, all passed ===`.
* `./health-check.sh all`: `All health checks passed!`
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0,
  `Success: All GET requests returned valid binary favicon data`.

Round 91–94 — в одном рабочем дереве (не коммитились раздельно).

## Задел

Остаются: `misc-include-cleaner` по оставшимся файлам, `modernize-avoid-c-arrays`
(64, без автофикса), `modernize-use-nodiscard` (14), `readability-identifier-naming`
(7), `modernize-use-ranges` (4).

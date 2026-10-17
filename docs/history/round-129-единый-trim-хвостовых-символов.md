# round 129 — единый trim хвостовых символов (`string_utils::trim_trailing`)

## Что сделано

Один и тот же цикл «снять хвостовые символы» был скопирован в двух местах:

- `crash_utils::trim_line(std::string)` (`crash_utils.hpp`) — снимает `\n`/`\r`
  (хвост строк addr2line);
- `PostgresQueryExecutor::Impl::trim_copy(const char *)`
  (`db_query_executor_postgres.cpp`) — `nullptr` → пусто, иначе снимает
  `\n`/`\r`/пробел (хвост текста ошибки libpq).

Общий примитив вынесен в `string_utils` (там же, где уже живёт `to_lower`):

```cpp
// string_utils.hpp
[[nodiscard]] std::string trim_trailing(std::string_view s,
                                        std::string_view chars = "\r\n");
```

Реализация — в `string_utils.cpp` (не `inline` в заголовке: так закрывающая
скобка не попадает в отчёт gcov как непокрытая строка — тот же приём, что и у
`to_lower`).

Обе точки стали тонкими адаптерами без дублирования цикла:

- `crash_utils::trim_line(std::string_view)` — `return trim_trailing(s);` (публичное
  имя и `[crash-utils]`-тесты сохранены; параметр переведён с `std::string` by value
  на `std::string_view`, т.к. копия для `pop_back` больше не нужна);
- `Impl::trim_copy` — `text == nullptr ? std::string() : trim_trailing(text,
  "\r\n ")` (проверка `nullptr` специфична для `PQerrorMessage` и остаётся на
  месте; попутно исчезает отдельная непокрытая ветка `return "";`).

Поведение не менялось: набор снимаемых символов и порядок сохранены. Попутно в
`crash_utils.hpp` добавлены прямые `<string_view>` и `<sys/types.h>` (последний —
закрывает предсуществующее `misc-include-cleaner`-предупреждение про `ssize_t`,
всплывшее, когда файл впервые попал в набор изменённых для clang-tidy).

## Проверка

- `./rebuild-and-run.sh` — rc=0.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто.
- coverage (`./scripts/run-coverage.sh`) — после правки гейт показал `REGRESSION
  crash_utils.hpp: 96.88% -> 96.67% (-0.21 п.п.)`. Это арифметика: тело
  `trim_line` сжалось на 2 **покрытые** строки, а единственная непокрытая строка
  файла (`self_exe_path`, `return "";` при `len <= 0` — на Linux `readlink`
  всегда успешен) осталась; процент упал, потому что файл стал короче.
  Прочие production-файлы: `db_query_executor_postgres.cpp` 202/215 → 196/208
  (94.0% → 94.2%), `string_utils.cpp` 6/7 → 11/12 (новая `trim_trailing`
  полностью покрыта). Baseline переанкорен
  (`coverage-regression-check.py --update`), повторная проверка — `OK: регрессий
  нет`, общий production 6516/7130 = 91.39%.

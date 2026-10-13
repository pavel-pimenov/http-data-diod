# Round 97: clang-tidy — `misc-include-cleaner` добит (422 → 0), весь бэклог закрыт

## Что сделано

Последний оставшийся чекер бэклога round 90 — `misc-include-cleaner` —
доведён до **нуля**. После раунда полный дет-свип (`run-clang-tidy.sh --all`)
даёт **0** предупреждений и ошибок по всем 45 проектным `.cpp` — бэклог
clang-tidy полностью закрыт.

| чекер | было (после 96) | стало |
| --- | --- | --- |
| misc-include-cleaner | 422 | 0 |
| все остальные | 0 | 0 |

## Подход

Находки делились на два вида: «`no header providing X is directly included`»
(393) и «`included header ... is not used directly`» (29). Прогон автофикса
по каждому виду:

```
clang-tidy -p /app/build-lint --checks="-*,misc-include-cleaner" \
           --fix --fix-errors <файлы>
```

* production `.cpp` — 32 файла (изменены 19);
* тестовые TU `test_*.cpp` — 13 файлов (изменены 12).

Итого затронут 31 файл (+234 / −29 строк). Правки — только в наборах
`#include`, логика не менялась.

## Ложное срабатывание: `request_handler.cpp` и `nats_client.hpp`

include-cleaner снял `nats_client.hpp` из `request_handler.cpp`, и сборка
упала с `invalid use of incomplete type 'class NatsClient'` (6 мест: 169, 170,
179, 855, 862, 864). Это тот же класс ошибки, что в round 90 для
`app_context.cpp`: `NatsClient` живёт в `std::shared_ptr` и вызывается через
`->`, имя типа текстуально не встречается — чекер считает include лишним.
Решение идентичное — вернуть include и пометить:

```cpp
#include "nats_client.hpp" // IWYU pragma: keep (NatsClient via shared_ptr needs complete type)
```

`nats_push_service.cpp` тоже потерял `nats_client.hpp`, но там `NatsClient`
не используется вообще (проверено grep) — удаление корректно.

## `main.cpp`: публичный заголовок вместо внутреннего

Автофикс для `spdlog::default_logger()` подставил `<spdlog/spdlog-inl.h>`
(именно там определена inline-функция). Заменил на публичный
`<spdlog/spdlog.h>` — чекер его принимает (повторный прогон по `main.cpp`:
0 находок), а зависимость от `-inl` внутренностей убрана.

## Замечания по стилю включений

* Чекер требует системные «C»-заголовки (`<stdlib.h>`, `<signal.h>`,
  `<sys/types.h>`, `<fcntl.h>`) там, где используются POSIX-символы
  (`setenv`/`unsetenv`, `kill`, `open` и т.п.) — `<cstdlib>`/`<csignal>` их
  не считает провайдерами, поэтому автоправки местами перевели `<cstdlib>`
  → `<stdlib.h>`. Оставлено как требует чекер (консистентно с round 90).
* В тестах, которые намеренно лезут во внутренности spdlog (кастомный
  `spdlog::formatter`, `level::level_enum`, `source_loc`, `memory_buf_t`),
  добавлены реальные заголовки spdlog (`formatter.h`, `common.h`,
  `common-inl.h`).
* Порядок include после вставок не отсортирован (clang-format в pre-commit
  не включён, гейта на сортировку нет) — оставлено в детерминированном
  порядке автоправки.

## Проверка

* `./rebuild-and-run.sh`: rc=0, `All tests passed` (2585 assertions in 556
  test cases) + (883 assertions in 120 test cases), `Done! Services are
  running` (ошибки в логе — ожидаемый вывод теста NATS CA-cert / Connection
  Closed).
* `./scripts/run-clang-tidy.sh --all`: **0** warnings/errors, без retry
  (OOM-фикс round 90 отработал штатно).
* `./health-check.sh all`: all passed.
* `python3 message_counter.py --iterations 1 --concurrent 1`: успешно.

## Итог по бэклогу clang-tidy

Round 88 включил 13 чекеров, round 90 вскрыл истинный бэклог (1141) и починил
sweep, round 91–97 последовательно закрыли все чекеры:

```
braces(91) → scoped-lock(92) → const-correctness(93) → raw-string(94)
→ nodiscard(95) → c-arrays + остаток(96) → include-cleaner(97) = 0.
```

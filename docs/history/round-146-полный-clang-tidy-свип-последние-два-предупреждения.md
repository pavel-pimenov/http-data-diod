# round 146 — полный clang-tidy-свип: последние два предупреждения

## Что сделано

Впервые за много раундов прогнали **полный** свип линтера
(`./scripts/run-clang-tidy.sh --all`, 47 TU) — до этого гейт линтил только
изменённые файлы, поэтому давно не тронутые TU никто не проверял. Свип нашёл
ровно **два** предупреждения в проектных файлах, оба в одном файле
`src/test_pg_value_utils.cpp`:

1. `google-build-using-namespace` — `using namespace pg_value_utils;`
2. `misc-include-cleaner` — «no header providing "json" is directly included»

Оба устранены без единого `NOLINT` — по конвенции проекта (см. такой же
комментарий в `test_proxy_core.cpp`):

```cpp
// было
using namespace pg_value_utils;

// стало
// Keep the import list explicit: no `using namespace` (google-build). The OID
// constants stay qualified at the call sites.
using pg_value_utils::hex_to_bytes;
using pg_value_utils::pg_type_name;
using pg_value_utils::pg_value;
```

Все 22 использования OID-констант в файле получили префикс
`pg_value_utils::` (строка таблицы `pg_type_name` осталась построчной: у
каждого REQUIRE свой диагностический контекст в отчёте Catch2).

Про `json` отдельный нюанс: `#include <nlohmann/json.hpp>` **не** гасит
предупреждение — include-cleaner ищет заголовок, который объявляет именно
используемое имя `json`, а это алиас `using json = nlohmann::json;` из
`json_utils.hpp` (проверено эмпирически: с `<nlohmann/json.hpp>` предупреждение
осталось, с `"json_utils.hpp"` — ушло). Поэтому подключён `"json_utils.hpp"` —
ровно тот заголовок, который объявляет имя.

Попутно три строки, переставшие влезать в 80 колонок после квалификации
констант, аккуратно перенесены.

После правки `./scripts/run-clang-tidy.sh --all` (повторный прогон) даёт **ноль**
предупреждений в проектных файлах — полный свип впервые полностью чистый.

## Попутно: склейка строк в HISTORY.md

Запись раунда 145 при вставке строки в таблицу положила литеральные `\n` внутрь
markdown-строки, из-за чего строки раундов 145, 144 и 143 оказались склеены в
одну физическую строку файла (в таблицу это не влияло, но `grep`/`sed` по
истории видели одну строку). Разбито обратно на три строки.

## Проверка

- `./rebuild-and-run.sh` — rc=0, `All tests passed (2813 assertions in
  589 test cases)`.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck (`./scripts/run-cppcheck.sh`) — чисто.
- clang-tidy — чисто; `--all` (47 TU) — ноль предупреждений в проектных файлах.
- coverage (`./scripts/run-coverage.sh`) — rc=0, `OK: регрессий покрытия нет`;
  правка только в `test_`-файле, production 6529/7143 = 91.40% без изменений.

# Round 90: clang-tidy — misc-include-cleaner + критичный фикс sweep (OOM)

## Главное: найден баг в `scripts/run-clang-tidy.sh` — «чистые» свипы 88/89 были ложными

Скрипт линтил файлы через `xargs -P N` (по числу CPU хоста, 12), а вывод
всех процессов писался в **один** поток (`> "$log" 2>&1`). В контейнере
Docker выделено ~8 ГБ памяти, а каждый clang-tidy TU заново парсит
вендорные заголовки и легко пикует выше 1 ГБ.

Эксперимент (12 файлов, `-P 12`, у каждого свой лог + rc):

```
rc=137 file=/app/app_context.cpp   lines=1     # 137 = 128+9 = SIGKILL (OOM)
rc=137 file=/app/common_utils.cpp  lines=1
rc=137 file=/app/duplicate_detector.cpp lines=1
rc=1   file=/app/http_client.cpp   lines=7853
...
```

`rc=137` — воркера убил OOM killer, при этом его диагностика **молча
терялась**: `|| true` глушил ненулевой код, а `grep` по усечённому выводу
ничего не находил. Итог: свипы round 88 и round 89 показывали
`✓ no errors or warnings`, хотя настоящий бэклог — **1141 находка**.

Полный последовательный прогон (`CLANG_TIDY_JOBS=1`, детерминированный)
даёт истинную картину:

| checker | кол-во |
| --- | --- |
| misc-include-cleaner | 424 |
| misc-const-correctness | 383 |
| readability-braces-around-statements | 167 |
| modernize-avoid-c-arrays | 64 |
| modernize-use-scoped-lock | 47 |
| modernize-raw-string-literal | 20 |
| modernize-use-nodiscard | 14 |
| readability-identifier-naming | 7 |
| modernize-use-ranges | 4 |
| readability-simplify-boolean-expr | 3 |
| readability-redundant-inline-specifier | 3 |
| readability-container-contains | 3 |
| readability-else-after-return | 1 |
| performance-unnecessary-copy-initialization | 1 |
| **всего** | **1141** |

### Фикс скрипта

* Каждый TU пишет **свой** лог и rc-файл (`/logs/<key>.log`, `/logs/<key>.rc`)
  — убитый воркер больше не может «съесть» чужую диагностику.
* Файлы с rc отсутствующим или `>= 128` (SIGKILL/OOM) **повторяются
  последовательно** — последовательный прогон гарантированно укладывается
  в лимит памяти.
* Логи агрегируются в исходном порядке файлов (стабильный вывод).
* Авто-`jobs` ограничен `min(CPU, 4)`, чтобы снизить частоту OOM;
  `CLANG_TIDY_JOBS` переопределяет.
* Log-каталог создаётся в `$BUILD_DIR` (внутри репозитория, gitignored):
  `mktemp -d` по умолчанию даёт `$TMPDIR=/var/folders/...`, который Docker
  Desktop на macOS **не расшаривает** → пустой mount (эта же ловушка была
  бы и на Linux-CI с иным TMPDIR, поэтому каталог явно в проекте).

Проверка: `./scripts/run-clang-tidy.sh --all` (jobs=4) теперь выдаёт ровно
**1141** предупреждение — совпадает с последовательным эталоном, retry-ев 0.

## misc-include-cleaner

В `src/.clang-tidy` включён `misc-include-cleaner` и добавлена опция
`misc-include-cleaner.IgnoreHeaders: 'httplib\.h;postgres_ext\.h;libpq-fe\.h;base64\.hpp;dpi\.h'`
— гасит находки от вендорных провайдеров (`Oid`→`postgres_ext.h`,
`httplib::*`→`httplib.h`), которые править нельзя.

Исправлены include-наборы в 11 `.cpp` (добавлены прямые провайдеры
используемых символов, убраны неиспользуемые):

* `circuit_breaker.cpp`: `+logger.hpp, +time_utils.hpp, +<cstdint>, +<prometheus/gauge.h>`.
* `config.cpp`: `-<cctype>, -<sstream>` (не используются), `+<exception>, +<string>`.
* `db_query_executor_base.cpp`: `+config.hpp, +json_utils.hpp, +<cstddef>, +<cstdint>, +<prometheus/family.h>, +<prometheus/gauge.h>, +<string>`.
* `db_query_executor_factory.cpp`: `+config.hpp, +<memory>`.
* `db_query_executor_oracle.cpp`: `+config.hpp, +db_query_executor_base.hpp, +<atomic>, +<cstdlib>, +<memory>`.
* `db_query_executor_postgres.cpp`: `+config.hpp, +db_query_executor_base.hpp, +<cstddef>, +<exception>, +<memory>`.
* `db_query_handler.cpp`: `+config.hpp, +db_query_executor.hpp, +json_utils.hpp, +<cstdint>, +<mutex>, +<prometheus/family.h>, +<prometheus/gauge.h>, +<utility>, +<vector>`.
* `duplicate_detector.cpp`: `+nlohmann/json.hpp, +<cstddef>, +<cstdint>, +<mutex>, +<string>, +<string_view>, +<vector>`.
* `http_client.cpp`: `+url_utils.hpp, +<atomic>, +<chrono>, +<cstdint>, +<memory>, +<stdexcept>, +<string>`.
* `http_client_pool.cpp`: `-<thread>` (не используется), `+http_client.hpp, +<cstddef>, +<memory>, +<mutex>, +<string>, +<utility>`.
* `app_context.cpp`: `+<memory>, +<prometheus/registry.h>`.

### Ложное срабатывание include-cleaner (важно)

include-cleaner пометил `rate_limiter.hpp`/`rate_limiter_per_ip.hpp`/
`duplicate_detector.hpp`/`trace_logger.hpp` в `app_context.cpp` как
«неиспользуемые». Это **ложное срабатывание**: `app_context.hpp`
форвард-декларирует эти типы и держит их в `std::unique_ptr`
(строки 185–189, 206–207, 225, 242), а деструктор `AppContext` определён
вне строки — значит `app_context.cpp` обязан видеть **полные** типы.
Чекер не видит инстанцирование деструктора `unique_ptr` через форвард-объявление.
Решение — оставить include и пометить `// IWYU pragma: keep` (причина
зафиксирована комментарием в файле). Убирать эти include нельзя — сборка
падает с `invalid application of 'sizeof' to incomplete type`.

## Побочные const-фиксы (выявлены детерминированным свипом)

Детерминированный прогон обнажил несделанные в round 89 const-правки в
заголовках (их прятал тот же OOM-баг):

* `in_flight_tracker.hpp`: `const std::scoped_lock`, `const bool completed`.
* `metrics_history.hpp`: `const std::scoped_lock` (×4), `const std::span` (×2).
* `rate_limiter.hpp`: `const std::scoped_lock`.
* `rate_limiter_per_ip.hpp`: `const std::scoped_lock` (×6), `const std::shared_ptr<RateLimiter>`, `const uint64_t` (×2).

## Проверка

* `./rebuild-and-run.sh`: rc=0 (после фикса восстановленных include в
  `app_context.cpp`), `All tests passed (883 assertions in 120 test cases)` +
  `(2585 assertions in 556 test cases)`, `Done! Services are running`.
* `./health-check.sh all`: `All health checks passed!`.
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0,
  `Success: No message loss or crossed responses detected`.
* `./scripts/run-clang-tidy.sh --all`: детерминированный, 1141 предупреждение
  (non-blocking) — задокументированный бэклог для следующих раундов.

## Задел (следующие раунды)

Бэклог 1141 находки разбирается по чекерам (каждый — отдельный раунд с
прогоном уже исправленного детерминированного sweep):
include-cleaner (424) · const-correctness (383) · braces (167) ·
avoid-c-arrays (64) · scoped-lock (47) · raw-string (20) · nodiscard (14) и т.д.

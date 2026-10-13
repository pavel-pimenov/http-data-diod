# Round 96: clang-tidy — закрыт весь бэклог кроме misc-include-cleaner

## Что сделано

Добит полный бэклог round 90/95, кроме `misc-include-cleaner` (422 — отдельная
тема). После раунда det-свип показывает **только** `misc-include-cleaner`
(422 находки), все остальные чекеры — 0.

| чекер | было (после 95) | стало |
| --- | --- | --- |
| modernize-avoid-c-arrays | 64 | 0 |
| misc-const-correctness | 9 | 0 |
| readability-identifier-naming | 8 | 0 |
| readability-simplify-boolean-expr | 3 | 0 |
| modernize-use-ranges | 4 | 0 |
| readability-container-contains | 3 | 0 |
| readability-redundant-inline-specifier | 3 | 0 |
| readability-else-after-return | 1 | 0 |
| performance-unnecessary-copy-initialization | 1 | 0 |
| misc-include-cleaner | 424 | 422 |

## modernize-avoid-c-arrays (вручную, 26 уникальных мест)

Автофикс чекера не использовался (даёт некрасивые `std::array<char, N>` по
строкам-константам). Сгруппированы по паттернам:

**Строковые константы → `std::string_view`** (никаких лишних массивов):
- `common_utils.cpp` `g_hex[]` → `std::string_view g_hex`.
- `sentry_client.cpp` `k_hex_digits[]` → `std::string_view` (`+<string_view>`).
- `trace_logger.cpp` `g_hex_chars[]` → `std::string_view` (`+<string_view>`).
- `server_handler.cpp` `g_correlation_test_header[]` → `constexpr const char*`
  (httplib `has_header/get_header_value` принимают `std::string` — `string_view`
  бы не сконвертился).

**Стек-буферы → `std::array<char, N>`** (C-функции получают `.data()/`.size()`,
`strlen/strcmp` на `.data()`):
- `crash_handler.hpp`: `time_str[32]`, `filename[512]`, `pid_buf[16]`, `tmp[16]`,
  `addr_buf[32]`, `callstack[128]`, `frame_buf[64]`, `buf[64]`, `in_path[256]`,
  `out_path[256]`, `line[512]`, `addr[32]` (×2), `sbuf[512]` + статический
  `CrashFrames::m_addrs[128]` → `std::array<void *, kMaxFrames>`.
  Всё по-прежнему stack-only, без аллокаций — async-signal-safety сохранена.
- `crash_utils.hpp`: `self_exe_path()` `buf[4096]` → `std::array` (`+<array>`).
- `stats_page.hpp`: `tsbuf[64]` → `std::array` (`+<array>`), вывод в HTML —
  `.data()`.
- `nats_client.cpp`: `connected_url[256]` → `std::array` (`+<array>`), у
  `natsConnection_GetConnectedUrl` теперь `.data()/.size()`.
- `common_utils.cpp`: `unsigned char hash[SHA256_DIGEST_LENGTH]` →
  `std::array`; range-for по hash работает как прежде.

**Тестовые таблицы:**
- `test_db_live.cpp`: анонимный `const struct {...} cases[]` → именованный
  `struct MalformedRequest { json m_request; std::string m_expect; }` +
  `std::vector<MalformedRequest>` (у `std::array<MalformedRequest, 8>` сломался
  агрегатный init — clang: `excess elements in struct initializer`; braced-list
  через `initializer_list` вектора работает).
- `test_nats_live.cpp`: `kNatsServerCandidates[]` → `std::array<const char*, 2>`.

## misc-const-correctness (7)

`crash_handler.hpp` `a/b/c/d` (random event_id) → `const uint32_t`; три
`restore_shutdown` в `test_nats_live.cpp`; два `lock` в `test_trace_logger.cpp`;
один `lock` в `test_coverage_ext.cpp` → `const std::scoped_lock`.

## readability-identifier-naming (8)

- `l2_worker.cpp:448` локальный `static ... const validator` →
  `g_validator` (правило `StaticConstantPrefix: g_`).
- `test_db_live.cpp`: члены `request/expect` → `m_request/m_expect`.
- `test_nats_live.cpp`: члены `PgTarget` `ok/need/uid/gid/name` → `m_*`;
  агрегатная инициализация позиционная, не сломана.

## readability-simplify-boolean-expr / use-ranges / и пр. (автофикс + фиксы)

Автофикс дал 14 строк в 8 файлах, после него вручную починено:
- **поломанное форматирование** else-after-return в `l2_worker.cpp` (clang-tidy
  снял `else {` и «приклеил» к `}` return — переформатировано вручную);
- **дубль `<algorithm>`** в `l2_worker_nats.cpp` (автофикс добавил второй
  include — убран);
- **тавтология** в `test_trace_logger.cpp` `if (r || !r)` → функция вызывается,
  а счётчик всегда инкрементируется (первая попытка `r ? 1 : 0` уронила тест
  «should_sample is thread-safe»: 218 != 400 — считались только `true`;
  переписано через `static_cast<void>(should_sample())` + `fetch_add(1)`).

`modernize-use-ranges` (4): `std::ranges::find/sort` в `l2_worker_nats.cpp`,
`test_app_context.cpp`, `test_db_live.cpp`, `test_nats_live.cpp`.

## Побочный фикс: `json_schema_validator.hpp` не был self-contained

Изменённый заголовок в changed-files режиме линкуется как отдельный TU, и
выяснилось, что ветка `std::flat_set` (C++23) **не включала `<flat_set>`** —
работала только на GCC за счёт транзитивного включения из другого стандартного
заголовка (clang standalone: `no template named 'flat_set'`). Добавлено
условное `#include <flat_set>` на верхнем уровне (вне класса) под тем же
guard'ом `__has_include(<flat_set>) && defined(__cpp_lib_flat_set)`; при
отсутствии C++23-библиотеки берётся `std::unordered_set`. Теперь заголовок
компилируется и как самостоятельный TU.

## Проверка

- `./scripts/run-clang-tidy.sh --all`: rc=0, находки только
  `misc-include-cleaner` (422).
- `./rebuild-and-run.sh`: rc=0, `All tests passed (2585 assertions in 556 test
  cases)` + `(883 assertions in 120 test cases)`.
- `./health-check.sh all`: `All health checks passed!`.
- `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0.

## Покрытие

Правки не меняют исполняемых строк/ветвей (замена типов и форм записи,
`std::array` vs C-массив — одинаковый код). `docs/coverage-baseline.json` не
менялся.

## Задел

Остаётся один чекер: `misc-include-cleaner` (422). До этого — фикс автогенерируемых
include, требует выборочного прогона по файлам и ручной сверки ложных
срабатываний (как `IWYU pragma: keep` в `app_context.cpp` из round 90).
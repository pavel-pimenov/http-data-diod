# Раунд 264: вендоринг fmt 12.2.1 и spdlog 1.17.0 (header-only) вместо apt

## Цель

Проект тянул spdlog из `libspdlog-dev` (apt), а fmt — из системного `libfmt10` (apt).
В apt-репозитории версии этих либ старее, чем актуальные (fmt 12.2.1, spdlog 1.17.0),
поэтому обе либы заведены в дерево проекта как вендорные, чтобы не зависеть от
версий apt и использовать свежие релизы.

## Что сделано

### 1. Вендоринг fmt 12.2.1

Архив `fmt-12.2.1.tar.gz` распакован, в `src/fmt/` перенесены только:
- `include/fmt/*.h` (15 заголовков: args, base, chrono, color, compile, core,
  fmt-c, format-inl, format, os, ostream, printf, ranges, std, xchar)
- `LICENSE`

`src/*.cc` (fmt.cc, fmt-c.cc, format.cc, os.cc) **не переносятся** — fmt собирается
в header-only режиме (`FMT_HEADER_ONLY`), компилируемые части не нужны и не
линкуются.

### 2. Вендоринг spdlog 1.17.0

Архив `spdlog-1.17.0.tar.gz` распакован, в `src/spdlog/` перенесены только:
- `include/spdlog/*` (включая `fmt/bundled/`, но он не используется — берём внешний fmt)
- `LICENSE`

`src/*.cpp` (async, bundled_fmtlib_format, cfg, color_sinks, file_sinks, spdlog,
stdout_sinks) **не переносятся** — spdlog используется header-only
(`spdlog::spdlog_header_only`).

### 3. CMakeLists.txt

Блок `find_package(spdlog CONFIG REQUIRED)` заменён на вендорные INTERFACE-targets:

```cmake
# fmt: header-only, FMT_HEADER_ONLY
add_library(fmt_header_only INTERFACE)
target_include_directories(fmt_header_only INTERFACE ${FMT_ROOT})
target_compile_definitions(fmt_header_only INTERFACE FMT_HEADER_ONLY)
add_library(fmt::fmt ALIAS fmt_header_only)

# spdlog: header-only против вендорного fmt через SPDLOG_FMT_EXTERNAL
add_library(spdlog_header_only INTERFACE)
target_include_directories(spdlog_header_only INTERFACE ${SPDLOG_ROOT})
target_compile_definitions(spdlog_header_only INTERFACE SPDLOG_FMT_EXTERNAL)
target_link_libraries(spdlog_header_only INTERFACE fmt_header_only)
add_library(spdlog::spdlog_header_only ALIAS spdlog_header_only)
```

Все существующие ссылки на `spdlog::spdlog_header_only` (l2-proxy, test_components,
test_proxy_core, PCH-таргеты) работают через alias без правок.

### 4. Dockerfile и Dockerfile.test

- Убраны `libfmt10` из runtime-стадии и `libspdlog-dev` из builder-стадии.
- fmt и spdlog вендорятся header-only → ни `libfmt10`, ни `libspdlog` не нужны
  как runtime-зависимости (раньше `libfmt10` был runtime-dep, а spdlog — только
  header-only).
- `libfmt-dev` и раньше не ставился (тянулся транзитивно через libspdlog-dev);
  теперь и `libspdlog-dev` не нужен.

### 5. VENDORED-LIBS.md

Добавлены строки в таблицу для fmt и spdlog; раздел «Обновление» дополнен
примечанием, что fmt/spdlog обновляются вручную из tar-архивов (не через
`update-vendored-libs.py`, который работает с git-клонами).

## Важные решения

- **Header-only vs compiled**: выбран header-only вариант — минимум файлов
  (только `include/` + LICENSE), нет сборки/линковки .cc/.cpp либ, нет
  runtime .so. spdlog уже использовался header-only; fmt переведён на
  `FMT_HEADER_ONLY`, поэтому `src/format.cc` не компилируется.
- **Внешний fmt 12.2.1 vs bundled 12.1.0**: выбран внешний fmt 12.2.1
  (`SPDLOG_FMT_EXTERNAL`), т.к. он новее bundled (12.2.1 > 12.1.0) и использует
  оба загруженных архива.
- `logger.hpp` использует `fmt::format_string`/`fmt::memory_buffer` — они приходят
  транзитивно через spdlog (`#include <fmt/format.h>`), пути подтягиваются через
  INTERFACE-target `fmt_header_only`.

## Проверка

Пересборка через `./rebuild-and-run.sh` + `python3 message_counter.py
--iterations 1 --concurrent 1` + `health-check.sh all` — все rc=0.

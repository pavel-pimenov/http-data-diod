# round 178 — чистка мёртвых зависимостей: CLI11, gperftools в базовом слое, мёртвый счётчик распаковки

Продолжение разбора `docs/dependency-analysis.md` (срез зависимостей от утра). В этом
раунде сделаны первые три пункта из «порядка работ» — дешёвые и с риском ≈0. Пункты
4–6 (экспозер на httplib вместо civetweb, `Logger` на `std::format`, mbedTLS вместо
OpenSSL) по договорённости отложены и в этом раунде не трогались.

## Что было не так

**1. `libcli11-dev` — пакет без единого использования.** CMake требовал его при
`BUILD_TESTS=ON` (`find_package(CLI11 REQUIRED)`), а `CLI11::CLI11` стоял в
`target_link_libraries(test_components)`. При этом ни один файл проекта не включал
CLI11: grep по `src/**` (`*.cpp/*.hpp/*.h/*.cc`, вне вендоренных каталогов) не находит
ни `CLI11`, ни `CLI::`. То есть каждый билд и каждый CI-прогон качал и ставил пакет
ради линковки с библиотекой, из которой не вызывается ни один символ.

**2. `libgoogle-perftools-dev` — всегда в builder-образе, нужен только профильным
сборкам.** CMake линкует `tcmalloc_and_profiler` только при `ENABLE_PROFILER=ON`
(опция по умолчанию `OFF`), а пакет ставился безусловно в базовый apt-слой builder'а.
Обычная сборка (и coverage-, и lint-слои, наследующие builder) платила за
профильный инструмент, который в ней не используется.

**3. `ProcessingErrorMetrics::m_decompression_errors` — мёртвый счётчик.** Член был
объявлен в структуре, «проводкой» привязан к категории `DECOMPRESSION_ERROR` в
`handle_processing_error_with_category`, но **нигде не регистрировался под именем и
ни разу не привязывался к реальному счётчику**: единственный продакшн-вызов
(`l2_worker.cpp`, «Request Schema Validation») передаёт `m_total_errors = nullptr`,
`m_json_errors` и `m_validation_errors` — распаковочный остаётся `nullptr`. Итог:
ветка existed только в коде, в `/metrics` не отражалась.

## Что сделано

**Пункт 1 — CLI11 выпилен из сборки:**

- `src/CMakeLists.txt`: удалён блок `find_package(CLI11 CONFIG QUIET)` +
  `find_package(CLI11 REQUIRED)`, удалён `CLI11::CLI11` из линковки
  `test_components`, в сообщение конфигурации вместо «Catch2 and CLI11» осталось
  «Catch2»;
- `src/Dockerfile`: из списка apt-пакетов builder'а убран `libcli11-dev`.

**Пункт 2 — gperftools ставится только для профильных сборок:**

- `src/Dockerfile`: `libgoogle-perftools-dev` убран из базового apt-слоя и вынесен в
  отдельный условный слой сразу после `ARG ENABLE_PROFILER`:
  `if [ "$ENABLE_PROFILER" = "true" ]; then apt-get install -y libgoogle-perftools-dev; fi`.
  Слой отдельный намеренно: базовый apt-слой (build-essential, cmake, postgresql,
  clang-tidy…) остаётся кэшируемым и одинаковым для обоих режимов, а переключение
  флага пересобирает только сам условный слой и следующие за ним шаги. Рантайм-образ
  не менялся: `libgoogle-perftools4t64` и до этого ставился только в отдельной стадии
  `runtime-profiler`.

**Пункт 3 — мёртвый счётчик убран:**

- `src/error_types.hpp`: удалён член `m_decompression_errors`;
- `src/common_utils.cpp`: из массива «категория → счётчик» убрана пара
  `DECOMPRESSION_ERROR` (теперь в нём только `JSON_PARSE_ERROR` и
  `VALIDATION_ERROR` — ровно те две категории, у которых есть выделенный счётчик).
  Категория `DECOMPRESSION_ERROR` никуда не делась: она по-прежнему определяется
  классификатором и печатается в логе как `[DECOMPRESSION_ERROR]`, а в метриках
  такие ошибки теперь попадают в счётчик «other» (в продакшне он не подключён — как
  и до раунда, ошибка видна в логе, но не в `/metrics`);
- `src/test_coverage_ext.cpp`: тест «maps JSON errors to the dedicated counter»
  переписан под новое поведение (счётчик `cu_decomp` убран, «bzip2 decompression
  failed» теперь увеличивает `cu_other`).

Попутно: `src/error_types.hpp` получил прямой `#include <string>` — clang-tidy
(`misc-include-cleaner`) указал, что `std::string` в заголовке используется без
собственного включения (замечание было скрыто тем, что файл давно не менялся и
не попадал в прогон линтера по изменённым файлам).

## Проверка

- **Сборка в контейнерах** (`./rebuild-and-run.sh`, rc=0): `All tests passed (2888
  assertions in 601 test cases)` — на 2 утверждения меньше, чем до раунда (2890):
  из теста убраны две проверки мёртвого счётчика и добавлены две проверки
  fallback-счётчика; `945 assertions in 125 test cases` — без изменений.
- `./health-check.sh all` — все проверки пройдены; `python3 message_counter.py
  --iterations 1 --concurrent 1` — rc=0, без потерь и перекрёстных ответов.
- `./scripts/ci-gate.sh runtime` (rc=0): метрики согласованы в четырёх источниках
  (`registered=102 dashboards=102 perses=102 readme=102 golden=102`), parity
  Grafana↔Perses — `OK`.
- `./scripts/ci-gate.sh offline` (rc=0): консистентность имён метрик, env-var ↔
  compose (99/99), docker build-context.
- `./scripts/run-cppcheck.sh` (rc=0) и `./scripts/run-clang-tidy.sh` (rc=0, «no errors
  or warnings in project files» — после добавления `<string>`).
- **Условный слой с gperftools проверен отдельно** (иначе сломанный профильный
  билд вскрылся бы только на специализированной сборке): сборка копии слоя с
  `ENABLE_PROFILER=true` даёт `/usr/lib/x86_64-linux-gnu/libtcmalloc_and_profiler.so`
  (ровно то, что ищет `find_library(TCMALLOC_LIBRARY tcmalloc_and_profiler)`), а с
  `ENABLE_PROFILER=false` пакет не ставится вовсе; в пересобранном
  `http-data-diod:builder` нет ни `libcli11-dev`, ни `libgoogle-perftools-dev`.
- **Вес:** A/B-замер apt-слоя builder'а на одинаковой базе — 1003.3 MB со старыми
  пакетами против 991.1 MB без них, то есть ≈12 MB экономии в образе сборки; в
  рантайм-образе (274 MB) изменений нет — там эти пакеты и не стояли.
- **Гейт покрытия (`scripts/run-coverage.sh`) на этом хосте показывает 7 «регрессий»,
  и все они объяснимы, реальной регрессии нет:**
  - шесть файлов (`config.cpp`, `http_client.cpp`, `http_client_pool.cpp`,
    `in_flight_tracker.hpp`, `header_utils.hpp`, `db_query_executor_oracle.cpp`)
    раундом **не тронуты** — это известный артефакт «baseline снят на arm64, а
    локальный прогон идёт на amd64» (тот же список и те же величины, что описаны
    в раунде 156: `config.cpp` 574 → 576 строк, `http_client.cpp` 97 → 101);
    гейт в CI считается на `ubuntu-24.04-arm`, то есть на архитектуре baseline;
  - седьмой файл — `common_utils.cpp`, единственный изменённый: 144/150 (96.00%) →
    142/148 (95.95%). Обе удалённые строки (пара `DECOMPRESSION_ERROR` в массиве
    «категория → счётчик») были **полностью покрытыми**, поэтому число непокрытых
    строк не изменилось — 6 до и 6 после; минус 0.05 п.п. — чистый эффект
    округления от уменьшения знаменателя, а не потеря покрытия.

## Что осталось (отложено по договорённости)

- Пункт 4 — экспозер `/metrics` на httplib вместо модуля `pull` (civetweb):
  −1 MB вендоренного C и отдельная C-сборка, риск средний, 3–4 ч.
- Пункт 5 — фасад `Logger` на `std::format` (без spdlog/fmt): −`libfmt10` в
  рантайм-образе, риск средний, 4–6 ч.
- Пункт 6 — mbedTLS вместо OpenSSL: отдельная задача, без профилирования не браться.

Отдельно зафиксированная находка (не правилась в этом раунде): у
`ProcessingErrorMetrics` в продакшне не подключены ни `m_total_errors`, ни
`m_other_errors` — единственный вызов передаёт для них `nullptr`, поэтому ошибки
обработки вне JSON/валидации видны только в логе. Стоит завести метрику «прочие
ошибки обработки» отдельным раундом (с синхронизацией четырёх источников).

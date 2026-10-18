# Round 98: cppcheck как полноценный гейт

## Что сделано

cppcheck раньше запускался один раз вручную (round 24, коммит `1b43d14`) и не
входил ни в pre-commit, ни в CI. Раунд превращает его в гейт, аналогичный
clang-tidy:

* `src/Dockerfile` — `cppcheck` добавлен в builder-слой apt (рядом с
  `clang-tidy`/`clang-format`).
* `scripts/run-cppcheck.sh` — новый скрипт, по интерфейсу повторяет
  `run-clang-tidy.sh`: `--all` (полный свип) или по умолчанию изменённые
  файлы, запуск в builder-контейнере, `CPPCHECK_JOBS` для `-j`.
* `src/cppcheck-suppressions.txt` — список подавлений вендорных заголовков.
* `scripts/pre-commit.sh` — шаг `run_cppcheck` (пропуск: `SKIP_CPPCHECK=1`).
* `.github/workflows/ci.yml` — джоба `cppcheck` (полный свип).
* `README.md` / `AGENTS.md` — документация инструмента.

## Параметры гейта

```
cppcheck --std=c++23 --enable=warning,performance,portability \
         --check-level=exhaustive --inline-suppr \
         --suppress=missingIncludeSystem \
         --suppressions-list=/app/cppcheck-suppressions.txt \
         -DL2_PROXY_BUILD_MODE=release \
         -I/app -I/app/httplib -I/app/base64 -I/app/odpi/include \
         -I/app/json -I/app/nats/src <проектные .cpp/.hpp>
```

Гейтятся только `error/warning/performance/portability` в проектных файлах.

### Почему не `--enable=all`

`style`/`information` намеренно исключены: cppcheck анализирует каждый TU
изолированно и потому массово даёт ложные `unusedFunction` /
`unusedStructMember` для символов, используемых из других TU, плюс
`missingInclude`-шум. Именно эти ~225 находок round 24 пометил как false
positives. Гейт на `error/warning/performance/portability` даёт
детерминированный и осмысленный результат.

### Вендорные заголовки

`-i` исключает только входные файлы, но не заголовки, которые cppcheck
разбирает **транзитивно** (их `#include` — это второй аргумент). Поэтому
находки в вендоре гасятся списком `src/cppcheck-suppressions.txt`:

```
*:/app/json/*        # returnByReference в nlohmann/json
*:/app/base64/*      # preprocessorErrorDirective (#error endianness)
*:/app/nats/*  *:/app/httplib/*  *:/app/odpi/*  *:/app/prometheus-cpp/*
```

## Найденные и исправленные реальные проблемы

До правок гейт давал 3 находки в проектных файлах:

1. **`duplicate_detector.cpp:105` — `internalAstError`.** Инлайн-форма
   `for (... : m | std::views::filter(...))` ломает AST-парсер cppcheck 2.19
   (`'e' doesn't have a parent`). Локальный тест на scratch-файле подтвердил:
   та же конструкция с **вынесенным** view парсится чисто. View вынесен в
   именованную переменную (`auto duplicates_view = m_state.m_entries | ...`) —
   стиль ranges сохранён. Важно: переменная **без** `const` — у `filter_view`
   `begin()` не const, `const auto` не компилируется (`passing ... discards
   qualifiers`); clang-tidy `misc-const-correctness` это понимает и не придирается.
2. **`test_db_live.cpp:244,278` — `ignoredReturnValue`.** `redirect_to()`
   объявлен `[[nodiscard]] bool`, но результат игнорировался в дочернем
   процессе после `fork()`. Теперь сбой редиректа приводит к `_exit(127)`,
   как и сбой `drop_privileges` — и находка снята, и поведение корректнее.

## Попутно

`scripts/pre-commit.sh`: pre-existing shellcheck-style SC2181
(`if [ $? -eq 0 ]` после `git commit`) заменён на `if git commit ...; then` —
заодно убран скрытый баг с `set -e` (раньше неуспешный `git commit` завершал
скрипт до ветки else).

## Проверка

* `./scripts/run-cppcheck.sh --all`: rc=0, 0 находок (109 проектных TU).
* `./scripts/run-clang-tidy.sh --all`: rc=0, 0 предупреждений.
* `./rebuild-and-run.sh`: rc=0, тесты `All tests passed` (2585/556 + 883/120).
* `./health-check.sh all`: all passed.
* `python3 message_counter.py --iterations 1 --concurrent 1`: success.
* `./scripts/run-shellcheck.sh`: новый скрипт чист.

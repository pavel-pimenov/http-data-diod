# Vendored libraries

В этом каталоге (`src`) лежат сторонние библиотеки, встроенные
непосредственно в дерево проекта (без git submodule). Версии зафиксированы здесь,
чтобы при обновлении слепка было понятно, откуда и чего ожидать.

В дерево переносится только минимально необходимый для работы сервисов набор
исходников: тесты, бенчмарки, examples, `CMakeLists.txt` и `BUILD.bazel`
апстрима вендорению не подлежат.

Сборка слинкована с системным пакетом `libfmt10` (spdlog-header-only + внешний fmt),
остальные зависимости вендорятся либо берутся из apt (см. `Dockerfile`).

| Библиотека | Версия | Источник | Комментарий |
|---|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | single-header `json/nlohmann/json.hpp` | пин `v3.12.0` (@ 55f9368); локальная правка `json.hpp`/`json_fwd.hpp`: NOLINT-комментарии + GCC C++20 modules workaround; пакет `nlohmann-json3-dev` убран из apt |
| [prometheus-cpp](https://github.com/jupp0r/prometheus-cpp) | master @ 66b6159 | source tree `prometheus-cpp/` (core/pull/push/util) | пин — коммит 66b6159 (ветка master, после v1.3.0: в `CMakeLists` уже `VERSION 1.3.0`); вендорено — apt-пакет 1.0.x без `Family::Remove/Has`; `core_export.h` генерируется апстримным CMake, в git-дереве его нет |
| [civetweb](https://github.com/civetweb/civetweb) | ~1.16 (релизный tarball) | `prometheus-cpp/3rdparty/civetweb/` | нужен pull-экспозеру prometheus-cpp (в дерево prometheus-cpp не входит, подтягивается при сборке); снимок = релизный tarball v1.16 (последний тег), из `civetweb.c` убран `#include "handle_form.inl"`; при `NO_FILESYSTEMS` civetweb требует от встраивающей стороны `log_access`/`mg_cry_internal_impl` (своих файлов не поставляет, `#error` без них) — заглушки генерируются `src/CMakeLists.txt` в build-каталог |
| [nats.c (cnats)](https://github.com/nats-io/nats.c) | 3.14.0 | source tree `nats/` (src/) | пин `v3.14.0` (@ 6cb096a7); проект использует только классическое publish/subscribe без JetStream; локальная правка `CMakeLists.txt`: examples/test отключены опциями `NATS_BUILD_EXAMPLES`/`BUILD_TESTING` |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.59.0 | `httplib/httplib.h` + `httplib.cc` | пин `v0.59.0` (@ cf3693c); пара заголовок+реализация собирается из single-header штатным `split.py` этого же тега; реализация не правится (сторонняя либа) |
| [base64](https://github.com/tobiaslocker/base64) | master @ 8d96a2a | single-header `base64/base64.hpp` | репозиторий без тегов; пин — коммит ветки master (версии в коде нет) |
| [OPI-C (odpi)](https://github.com/oracle/odpi) | 26.0.0 | source tree `odpi/` | пин `v26.0.0`; `DPI_MAJOR_VERSION 26`, `DPI_VERSION_SUFFIX` пуст |

## Обновление

Проверка соответствия дерева пинам и апгрейд либ делаются скриптом:

```bash
python3 scripts/update-vendored-libs.py                          # отчёт
python3 scripts/update-vendored-libs.py --ref cpp-httplib=v0.56.1  # апгрейд + копирование
python3 scripts/update-vendored-libs.py --ref nlohmann-json=v3.13.0 --force  # апгрейд и патчей
python3 scripts/update-vendored-libs.py --prune                  # в апгрейде удалять исчезнувшие в апстриме файлы
```

Пины по умолчанию в скрипте (`scripts/update-vendored-libs.py`, `REFS`)
равны колонке «Версия». Запись на диск выполняется только для либ, у которых
ref явно переопределён через `--ref`; локальные правки (см. `PATCHED` в скрипте)
при этом не перезаписываются без `--force`. Для cpp-httplib скрипт сам запускает
`split.py` зафиксированного тега.

## Правила обновления

- Версии поднимаются только целиком на то же самое, что в апстриме.
- После обновления любой вендоренной либы: пересборка через `./rebuild-and-run.sh`
  + `python3 message_counter.py --iterations 1 --concurrent 1`, запись в `HISTORY.md`.
- Реализации вендоренных либ не редактируем (это слепки апстрима). Если нужен фикс —
  сначала убедиться, что он не пришёл в свежем апстриме.
- Локально пропатчены (правки эти нужно сохранить при апгрейде):
  `json/nlohmann/json.hpp`, `json/nlohmann/json_fwd.hpp` (NOLINT, GCC modules
  workaround), `nats/CMakeLists.txt` (примеры/тесты за опциями),
  `prometheus-cpp/3rdparty/civetweb/src/civetweb.c` (убран
  `#include "handle_form.inl"`).
- Файлы, которых нет в git-дереве апстрима, поэтому `--prune` их удаляет —
  перечислены в `PATCHED` скрипта (там же причина): `core_export.h` в
  prometheus-cpp генерируется его CMake. Заглушки civetweb
  (`external_log_access.inl`, `external_mg_cry_internal_impl.inl`) в вендоренном
  дереве не лежат — их пишет `file(WRITE)` в `src/CMakeLists.txt` в build-каталог,
  потому что апстрим их не поставляет вовсе.
- Либы без свежих релизов (`nlohmann/json`, `nats.c`, `odpi`, `civetweb`)
  обновляются только при появлении нового тега; `prometheus-cpp` и `base64`
  пинятся на коммит ветки master, поэтому сверяются с `git ls-remote`.
- odpi компилируется через `odpi/embed/dpi.c` (amalgamation — просто `#include`
  всех `src/*.c`), поэтому обновление файлов в `src/` не требует перегенерации.
- Пакеты из `apt` (если перестают нуждаться или, наоборот, теперь нужны) —
  сверять со списком в `Dockerfile` и разделом «сборка» в README.
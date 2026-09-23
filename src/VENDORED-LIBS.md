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
| [prometheus-cpp](https://github.com/jupp0r/prometheus-cpp) | master @ 00c1329 | source tree `prometheus-cpp/` (core/pull/push/util) | пин — коммит 00c1329 (ветка master, 29.08.2026); тегам v1.2.x не соответствует; вендорено — apt-пакет 1.0.x без `Family::Remove/Has` |
| [civetweb](https://github.com/civetweb/civetweb) | ~1.16 (снимок) | `prometheus-cpp/3rdparty/civetweb/` | нужен pull-экспозеру prometheus-cpp (в дерево prometheus-cpp не входит, подтягивается при сборке); локальный снимок не совпадает ни с одним тегом (макрос версии 1.16, часть файлов из более новых коммитов) |
| [nats.c (cnats)](https://github.com/nats-io/nats.c) | 3.14.0 | source tree `nats/` (src/) | пин `v3.14.0` (@ 6cb096a7); проект использует только классическое publish/subscribe без JetStream; локальная правка `CMakeLists.txt`: examples/test отключены опциями `NATS_BUILD_EXAMPLES`/`BUILD_TESTING` |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.58.0 | `httplib/httplib.h` + `httplib.cc` | пин `v0.58.0` (@ 4f3f9ef); пара заголовок+реализация собирается из single-header штатным `split.py` этого же тега; реализация не правится (сторонняя либа) |
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
  workaround), `nats/CMakeLists.txt` (примеры/тесты за опциями).
- odpi компилируется через `odpi/embed/dpi.c` (amalgamation — просто `#include`
  всех `src/*.c`), поэтому обновление файлов в `src/` не требует перегенерации.
- Пакеты из `apt` (если перестают нуждаться или, наоборот, теперь нужны) —
  сверять со списком в `Dockerfile` и разделом «сборка» в README.
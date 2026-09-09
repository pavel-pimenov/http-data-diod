# Vendored libraries

В этом каталоге (`cpp/l2-proxy`) лежат сторонние библиотеки, встроенные
непосредственно в дерево проекта (без git submodule). Версии зафиксированы здесь,
чтобы при обновлении слепка было понятно, откуда и чего ожидать.

Сборка слинкована с системным пакетом `libfmt10` (spdlog-header-only + внешний fmt),
остальные зависимости вендорятся либо берутся из apt (см. `Dockerfile`).

| Библиотека | Версия | Источник | Комментарий |
|---|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | single-header `nlohmann/json.hpp` | вендорен в `json/nlohmann/` (коммит eadbc1b); пакет `nlohmann-json3-dev` убран из apt |
| [prometheus-cpp](https://github.com/jupp0r/prometheus-cpp) | 1.2.4 | source tree `prometheus-cpp/` (core/pull/push/util) | пришлось вендорить — пакет Ubuntu 1.0.x без `Family::Remove/Has` |
| [nats.c (cnats)](https://github.com/nats-io/nats.c) | 3.14.0-beta (SHA 9cae373) | source tree `nats/src/` | апстрим-синк ветки `main`; проект использует только классическое publish/subscribe без JetStream |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.54.1 | single-header `httplib/httplib.h` | только интерфейс; реализация на C++26 не правится (сторонняя либа) |
| [base64](https://github.com/tobiaslocker/base64) | (latest, версии в коде нет) | single-header `base64/base64.hpp` | |
| [OPI-C (odpi)](https://github.com/oracle/odpi) | 26.0.0-b1 | source tree `odpi/` | Oracle Database Programming Interface; `DPI_MAJOR_VERSION 26`, `DPI_VERSION_SUFFIX "b1"` |

## Правила обновления

- Версии поднимаются только целиком на то же самое, что в апстриме.
- После обновления любой вендоренной либы: пересборка через `./rebuild-and-run.sh`
  + `python3 message_counter.py --iterations 1 --concurrent 1`, запись в `HISTORY.md`.
- Реализации вендоренных либ не редактируем (это слепки апстрима). Если нужен фикс —
  сначала убедиться, что он не пришёл в свежем апстриме.
- odpi компилируется через `odpi/embed/dpi.c` (amalgamation — просто `#include`
  всех `src/*.c`), поэтому обновление файлов в `src/` не требует перегенерации.
- Пакеты из `apt` (если перестают нуждаться или, наоборот, теперь нужны) —
  сверять со списком в `Dockerfile` и разделом «сборка» в README.
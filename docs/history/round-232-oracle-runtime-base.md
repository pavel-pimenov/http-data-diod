# Round 232 — Oracle OCI-клиент встроен в общий runtime-base (единый бинарник)

## Задача
Пользователь заметил: Oracle-клиент присутствовал только в opt-in-таргете
`runtime-db`, а l2-proxy/l2-server собирались в `runtime` без Oracle. Но в проекте
**один бинарник** `l2-proxy` для всех режимов (переключается переменной `MODE`:
proxy/worker/server). Раз worker-режим ходит в Oracle, OCI-клиент должен быть
влинкован в **общий** runtime-образ.

## Причина
- `runtime` / `runtime-asan` / `runtime-profiler` / `runtime-valgrind` наследовались
  от `runtime-base`, где Oracle не было.
- `runtime-db` (opt-in, с Oracle) собирался только для l2-worker через
  `L2_WORKER_DOCKER_TARGET=runtime-db`.
- Так как бинарник один, а режим задаётся `MODE`, любой контейнер мог работать
  worker'ом → Oracle должен быть во всех runtime-образах.

## Реализация
### `src/Dockerfile`
- Стадия `oracle-libs` (источник OCI из `gvenzl/oracle-xe:21.3.0-slim`) вынесена на
  верхний уровень **перед** `runtime-base` (избегая циклической зависимости: ранее
  `FROM ... AS oracle-libs` был объявлен внутри `runtime-base`, из-за чего
  `COPY --from=oracle-libs` в той же стадии давал `circular dependency detected on
  stage: oracle-libs`).
- Весь блок Oracle (копирование `libclntsh.so.21.1` + `libclntshcore` + `libnnz21`,
  zoneinfo, nls; apt `libaio1t64 libnsl2`; `ORACLE_HOME`; ld.so-регистрация и
  symlink `libaio.so.1 → libaio.so.1t64`) перенесён из `runtime-db` в **`runtime-base`**.
- `runtime-db` стал простым алиасом `FROM runtime AS runtime-db` (обратная совместимость).

### `docker-compose.yml`
- Комментарии обновлены: OCI-клиент встроен в общий `runtime-base`; `runtime-db` —
  алиас `runtime`.

### README.md / docs/http-db-gate-example.md
- Разделы про Oracle обновлены: Oracle-клиент в общем `runtime-base`, отдельный
  таргет не нужен.

## Проверка
- `docker build --check --target runtime` — синтаксис валиден, циклической
  зависимости нет.
- `./rebuild-and-run.sh` — сборка зелёная, все сервисы healthy.
- Oracle-клиент присутствует во **всех** runtime-образах: `l2-proxy`, `l2-worker`,
  `l2-server` (`/opt/oracle/instantclient_21_13/libclntsh.so` есть в каждом).
- `message_counter.py --iterations 1 --concurrent 1` — exit 0 (no message loss).

## Ключевое решение
Единый бинарник `l2-proxy` (режим через `MODE`) → Oracle OCI-клиент зашит в общий
`runtime-base`, чтобы любой режим (proxy/worker/server) мог подключаться к Oracle;
`runtime-db` сохранён как алиас `runtime`.

# HTTP DB Gateway — как это работает (пример)

Полный контур: **HTTP → l2-proxy → NATS (`service.db.query`) → l2-worker → СУБД (PostgreSQL libpq / Oracle ODPI-C pool) → обратно**.

По умолчанию подключён **PostgreSQL** (`DB_POSTGRES_ENABLED=true`), Oracle — опционально через profile `oracle` (`DB_ORACLE_ENABLED=true`).

Схема запроса `POST /v1/sql/oracle/query`:

```
client ──POST /v1/sql/oracle/query──▶ l2-proxy (8888)
                                          │  валидация, маршрутизация
                                          ▼
                                     NATS  service.db.query
                                          │  queue group db_workers
                                          ▼
                                      l2-worker
                                          │  DbQueryHandler → DbQueryExecutor (ODPI-C)
                                          ▼
                                      Oracle XE 21c  (oracle:1521/XEPDB1, user app_user)
```

## Топология

| Сервис | Роль | Что задействовано |
|--------|------|-------------------|
| `postgres` | БД | `postgres:16-alpine`, демо-схема из `sql/postgres.sql`. Включён по умолчанию (`DB_POSTGRES_ENABLED=true`) |
| `oracle` | БД | `gvenzl/oracle-xe:21.3.0-slim`, демо-схема из `sql/oracle.sql`. Запускается только через profile: `docker compose --profile oracle up -d` |
| `l2-proxy` | HTTP-вход | регистрирует БД из `DB_POSTGRES_*` / `DB_ORACLE_*` env, роутит `/v1/sql/*` в NATS |
| `l2-worker` | Исполнитель | образ `runtime-db` (Oracle OCI из XE-образа + libpq), держит пулы сессий, отвечает на `service.db.query` |
| `nats-server` | Транспорт | subject `service.db.query`, queue group `db_workers` |

## Переменные окружения (docker-compose.yml)

По умолчанию шлюз включён (`DB_QUERY_ENABLED=true`), PostgreSQL подключён, Oracle отключён (`DB_ORACLE_ENABLED=false`). Для запуска Oracle-контура поднимите сервис с профилем и включите Oracle:

```
docker compose --profile oracle up -d
DB_ORACLE_ENABLED=true docker compose up -d l2-worker l2-proxy
```

Полностью отключить шлюз: `DB_QUERY_ENABLED=false`.

- `DB_QUERY_NATS_SUBJECT` — subject запросов (default `service.db.query`)
- `DB_QUERY_NATS_QUEUE_GROUP` — queue group (default `db_workers`)
- `DB_QUERY_NATS_TIMEOUT_MS` — таймаут ожидания ответа воркера (default `30000`)
- `DB_QUERY_DEFAULT_TIMEOUT_MS` / `DB_QUERY_DEFAULT_MAX_ROWS` — дефолты запросов (5000 ms / 1000 строк)
- `DB_ORACLE_ENABLED`, `DB_ORACLE_HOST`, `DB_ORACLE_PORT`, `DB_ORACLE_SERVICE`, `DB_ORACLE_USER`, `DB_ORACLE_PASSWORD`, `DB_ORACLE_POOL_MIN`, `DB_ORACLE_POOL_MAX`
- `DB_POSTGRES_ENABLED`, `DB_POSTGRES_HOST`, `DB_POSTGRES_PORT`, `DB_POSTGRES_DB`, `DB_POSTGRES_USER`, `DB_POSTGRES_PASSWORD`, `DB_POSTGRES_POOL_MIN`, `DB_POSTGRES_POOL_MAX`

## Oracle OCI client: откуда библиотеки в образе

l2-worker ходит в Oracle через ODPI-C, которому нужны динамические библиотеки Oracle OCI
(`libclntsh.so` и др.). В исходниках их нет и с сети они **не качаются**: сборка `runtime-db`
извлекает их прямо из образа `gvenzl/oracle-xe:21.3.0-slim` (того же, что используется для
сервиса `oracle`) средствами multi-stage `COPY --from`. Это работает в закрытой сети — не нужен
ни `download.oracle.com`, ни отдельный Instant Client zip.

### Что и откуда копируется (Dockerfile, stage `oracle-libs` → `runtime-db`)

Стадия `oracle-libs` = `FROM gvenzl/oracle-xe:21.3.0-slim`, но только как «хранилище файлов»
(БД при сборке запускается). `runtime-db` копирует из `$ORACLE_HOME` = `/opt/oracle/product/21c/dbhomeXE`:

| Компонент | Путь в XE-образе | Зачем |
|---|---|---|
| `libclntsh.so.21.1` | `lib/libclntsh.so.21.1` | основная OCI-библиотека, её грузит ODPI-C (`dlopen`) |
| `libclntshcore.so.21.1` | `lib/libclntshcore.so.21.1` | зависимость `libclntsh` |
| `libnnz21.so` | `lib/libnnz21.so` | крипто/dep, тоже зависимость `libclntsh` |
| timezone | `oracore/zoneinfo/` | обязателен; без него клиент падает с `ORA-01804 failure to initialize timezone information` |
| NLS-данные | `nls/` | нужны полному (не-Instant-Client) OCI для инициализации нац. языков; без них `ORA-12715` |

Нужное в контейнере раскладывается в `/opt/oracle/instantclient_21_13`:

- создаются симлинки `libclntsh.so → libclntsh.so.21.1` и `libclntshcore.so →
  libclntshcore.so.21.1` (для `dlopen("libclntsh.so")`);
- задаётся `ENV ORACLE_HOME=/opt/oracle/instantclient_21_13` — полный OCI ищет zoneinfo/NLS
  относительно `$ORACLE_HOME`, а не только рядом с `libclntsh`;
- ставится `libaio1t64` + compat-symlink `libaio.so.1 → libaio.so.1t64` (`libclntsh` линкуется
  против legacy SONAME `libaio.so.1`; на Ubuntu 26.04 t64-переход) и `libnsl2`;
- каталог `/opt/oracle/instantclient_21_13` регистрируется в динамическом загрузчике через
  `/etc/ld.so.conf.d/oracle-instantclient.conf` + `ldconfig` (подробно — раздел ниже).

### Как встроить клиент в образ l2-worker

```
L2_WORKER_DOCKER_TARGET=runtime-db DB_ORACLE_ENABLED=true docker compose up -d --build l2-worker
```

вместе с profile `oracle`. По умолчанию сборка идёт с `target=runtime` и OCI-клиент **не**
кладётся в образ — так локальные/тестовые сборки с PostgreSQL не тянут лишние ~200 МБ.

Проверка, что клиент живой:

```
docker compose logs l2-worker | grep -i "DB executor 'oracle'"   # ждать "pool ready"
docker compose up -d --profile oracle                            # если oracle ещё не поднят
curl http://localhost:8888/v1/sql/oracle/ping                    # {"status":"ok",...}
```

## ld.so регистрация подробно (что делает runtime-db)

### Как ODPI-C вообще находит `libclntsh.so`

Встраиваемый ODPI-C не линкуется с клиентом напрямую — он делает `dlopen("libclntsh.so")` и потом,
по цепочке, `libclntsh.so.19.1`, `.18.1`, ..., вплоть до `.21.1` (`dpiOci.c`, массив `dpiOciLibNames`).
Динамический загрузчик glibc ищет библиотеку в таком порядке:

1. `$LD_LIBRARY_PATH` (если задан);
2. кэш `/etc/ld.so.cache`, который строит `ldconfig` из каталогов, перечисленных в
   `/etc/ld.so.conf` (+ всё из `/etc/ld.so.conf.d/*.conf`);
3. системные каталоги по умолчанию (`/lib`, `/usr/lib`).

Поэтому задача ld.so-регистрации — сделать так, чтобы `<каталог клиента>` попал в п.2 (кэш
`ldconfig`). Альтернатива — п.1 (`LD_LIBRARY_PATH`), см. ниже.

### Что делает каждый шаг ldconfig

- `/etc/ld.so.conf.d/oracle-instantclient.conf` — текстовый файл, обычно **одна строка** — путь к
  каталогу с `.so`: `/opt/oracle/instantclient_21_13`. Имена конфигов в этом каталоге должны иметь
  суффикс `.conf` (иначе `ldconfig` их не прочитает).
- `ldconfig`:
  - сканирует каталоги из `/etc/ld.so.conf` и `/etc/ld.so.conf.d/*.conf`;
  - смотрит **SONAME** каждой `.so` (например `libclntsh.so.21.1`) и создаёт/поправляет симлинки в
    самих каталогах под это имя;
  - пишет бинарный кэш `/etc/ld.so.cache`, по которому `dlopen` находит библиотеку по имени без
    сканирования диска.
  В кэш попадает как минимум запись по **SONAME** (`libclntsh.so.21.1`). Более короткое
  `libclntsh.so` — это symlink, который runtime-db создаёт в каталоге клиента; ODPI-C перебирает
  имена
  по цепочке `libclntsh.so`, `.19.1`, ..., `.21.1`, поэтому даже если bare-имя не попадёт в кэш,
  финальное имя `.21.1` из списка в кэше есть — `dlopen` гарантированно находит клиент.

### Проверка, что регистрация сработала

```bash
# 1) клиент виден в кэше загрузчика
docker compose exec l2-worker sh -c 'ldconfig -p | grep -iE "clntsh|aio"'

# 2) симлинки на месте: bare-имя libclntsh.so указывает на SONAME-файл
docker compose exec l2-worker sh -c 'ls -l /opt/oracle/instantclient_21_13/libclntsh.so; ls -l /usr/lib/x86_64-linux-gnu/libaio.so.1'

# 3) ODPI-C реально загрузил клиент — в логах воркера появилось
docker compose logs l2-worker | grep -i "DB executor 'oracle'"
```

Ожидаемый результат п.1 (ключевые строки; bare-имя `libclntsh.so` может подтянуться в кэш
дополнительной записью из-за symlink):

```
libclntsh.so.21.1       => /opt/oracle/instantclient_21_13/libclntsh.so.21.1
libclntshcore.so.21.1   => /opt/oracle/instantclient_21_13/libclntshcore.so.21.1
libaio.so.1             => /usr/lib/x86_64-linux-gnu/libaio.so.1
```

### t64-нюанс `libaio` (SONAME vs. файл)

`libclntsh.so.21.1` линкуется против SONAME **`libaio.so.1`**. На Ubuntu 26.04/24.04 (t64-переход)
пакет `libaio1t64` кладёт **`libaio.so.1t64`**, а привычного `libaio.so.1` нет. Абсолютно необходимый
мост — symlink `libaio.so.1 → libaio.so.1t64` (без него `dlopen` клиента упадёт с `libaio.so.1:
cannot open shared object file`). В Dockerfile этот symlink создаёт `runtime-db`:
`ln -sf /usr/lib/x86_64-linux-gnu/libaio.so.1t64 /usr/lib/x86_64-linux-gnu/libaio.so.1`.

### Альтернатива без ld.so — `LD_LIBRARY_PATH`

Если менять `/etc` не хочется, можно добавить каталог клиента в `LD_LIBRARY_PATH` процесса.
Задать его нужно **до старта** процесса l2-worker — например, в `docker-compose.yml` секции
l2-worker:

```yaml
environment:
  - LD_LIBRARY_PATH=/opt/oracle/instantclient_21_13
```

после чего `docker compose up -d --force-recreate l2-worker`. Внутри работающего контейнера
`export LD_LIBRARY_PATH=...` эффекта не даст — это переменная запущенного процесса, а не его
родителя.

Работает для `dlopen`, но менее «правильно»: `LD_LIBRARY_PATH` переопределяет системные пути для
всех lib, кэш `ldconfig` быстрее и надёжнее, а `ld.so.conf` покрывает и случаи, когда каталог
клиента подключается как volume.

### Почему правки в живом контейнере эфемерны → клиент в образе

`docker compose` (re)create контейнера пересобирает его из образа, и все ручные изменения
(`ld.so.conf.d`, симлинки, сам каталог `/opt/oracle`) теряются. Поэтому клиент кладётся в образ
при сборке — stage `oracle-libs` (источник из XE) + `runtime-db` (компоновка) это делают по
умолчанию. В закрытой сети достаточно, чтобы `gvenzl/oracle-xe:21.3.0-slim` был доступен
локальному Docker (он используется и сервисом `oracle`); отдельного файлового контура OCI нет —
весь контур собирается «из коробки». Если offline-хост вообще не умеет собирать образы, соберите
`runtime-db` один раз на машине с Docker и разворачивайте через `docker save`/`docker load`.

## Демо-данные

`sql/oracle.sql` (и `sql/postgres.sql` для PostgreSQL) создаёт таблицу `app_user.demo_messages` / `demo_messages` и заполняет её:

```
1 | Hello from Oracle DB gateway
2 | DB gateway works over NATS
```

## Как проверить (реальные команды и вывод)

### 1. Список баз данных

```bash
curl http://localhost:8888/v1/sql
```

```json
{"databases":[{"driver":"postgres","enabled":true,"name":"postgres"}]}
```
> При включённом Oracle (`DB_ORACLE_ENABLED=true`, profile `oracle`) список
> дополняется `{"driver":"oracle","enabled":true,"name":"oracle"}`.
>
> Пути `/v1/sql` и `/v1/sql/` эквивалентны (обрезаются ведущие/хвостовые слэши).

### 2. Ping базы

```bash
curl http://localhost:8888/v1/sql/oracle/ping
```

```json
{"db":"oracle","latency_ms":496,"status":"ok"}
```

### 3. Простой SELECT

```bash
curl -X POST http://localhost:8888/v1/sql/oracle/query \
  -H 'Content-Type: application/json' \
  -d '{"sql":"SELECT id, message, created_at FROM app_user.demo_messages ORDER BY id"}'
```

```json
{
  "status": "ok",
  "db": "oracle",
  "columns": [
    {"name": "ID", "type": "NUMBER"},
    {"name": "MESSAGE", "type": "VARCHAR2"},
    {"name": "CREATED_AT", "type": "TIMESTAMP"}
  ],
  "rows": [
    ["1", "Hello from Oracle DB gateway", "2026-08-09 12:29:57.060963000"],
    ["2", "DB gateway works over NATS",   "2026-08-09 12:29:57.220733000"]
  ],
  "row_count": 2,
  "truncated": false,
  "duration_ms": 817
}
```

### 4. Запрос с bind-переменной

```bash
curl -X POST http://localhost:8888/v1/sql/oracle/query \
  -H 'Content-Type: application/json' \
  -d '{"sql":"SELECT message FROM app_user.demo_messages WHERE id = :id", "params": {"id": 2}}'
```

```json
{
  "status": "ok",
  "db": "oracle",
  "columns": [{"name": "MESSAGE", "type": "VARCHAR2"}],
  "rows": [["DB gateway works over NATS"]],
  "row_count": 1,
  "truncated": false,
  "duration_ms": 160
}
```

### 4b. То же самое для PostgreSQL (включён по умолчанию)

```bash
curl -X POST http://localhost:8888/v1/sql/postgres/query \
  -H 'Content-Type: application/json' \
  -d '{"sql":"SELECT id, message FROM demo_messages ORDER BY id"}'
```

```json
{
  "status": "ok",
  "db": "postgres",
  "columns": [
    {"name": "id", "type": "int4"},
    {"name": "message", "type": "text"}
  ],
  "rows": [
    [1, "Hello from PostgreSQL DB gateway"],
    [2, "DB gateway works over NATS"]
  ],
  "row_count": 2,
  "truncated": false,
  "duration_ms": 12
}
```

### 5. Контракт методов

| Путь | HTTP-метод | Назначение | Некорректный метод |
|---|---|---|---|
| `/v1/sql` и `/v1/sql/` | `GET` | Список баз данных | `405 METHOD_NOT_ALLOWED` |
| `/v1/sql/{db}/ping` | `GET` | Проверка доступности БД | `405 METHOD_NOT_ALLOWED` |
| `/v1/sql/{db}/query` | `POST` | Выполнить SQL-запрос | `405 METHOD_NOT_ALLOWED` |

Неизвестный `{action}` (например `/v1/sql/oracle/bogus`) отвечает `404 NOT_FOUND`,
известный, но вызванный не тем HTTP-методом (например `GET /v1/sql/oracle/query`)
— `405 METHOD_NOT_ALLOWED`.

### 6. Ошибки

- `400 BAD_REQUEST` — невалидный JSON-тело `query` или невалидный контракт (пустое тело, нет `sql`)
- `404 NOT_FOUND` — неизвестный DB gateway путь / неизвестный action
- `404 UNKNOWN_DATABASE` — неизвестная база (`/v1/sql/mssql/query`)
- `405 METHOD_NOT_ALLOWED` — известный action, но не тот HTTP-метод
- `422 SQL_ERROR` — ошибка Oracle / не-read-only SQL (не начинается с `SELECT`/`WITH`)
- `504 TIMEOUT` — воркер не ответил за `DB_QUERY_NATS_TIMEOUT_MS`
- `503 DB_UNAVAILABLE` — пул недоступен (Oracle лежит)

## Логи воркера

При успешной активации шлюза l2-worker пишет:

```
DB executor 'oracle': pool ready (1..5 sessions, connect oracle:1521/XEPDB1 )
DB handler: ready with 1 database(s)
Worker subscribed to DB NATS subject: service.db.query (queue group db_workers)
```

Пока Oracle холодно стартует, воркер ретраит инициализацию пула, **не трогая** основную подписку:

```
DB gateway is not ready yet (Oracle unavailable?), will retry
```

## Известные ограничения

- Только read-only: текст запроса должен начинаться с `SELECT`/`WITH` (иначе 422).
- Поддерживаемые драйверы: PostgreSQL (libpq, включён по умолчанию) и Oracle (ODPI-C / OCI client из XE-образа, опционально через profile `oracle`).

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
| `l2-worker` | Исполнитель | образ `runtime-db` (Oracle Instant Client 21.13 + libpq), держит пулы сессий, отвечает на `service.db.query` |
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

## Oracle Instant Client: где скачать и куда подложить в контейнер

l2-worker ходит в Oracle через ODPI-C, которому нужны динамические библиотеки Oracle Instant Client
(`libclntsh.so` и др.). В исходниках они не лежат — их качает Dockerfile при сборке.

### Вариант 1 — автоматическая сборка (рекомендуемый)

Dockerfile (`cpp/l2-proxy/Dockerfile`, stage `oracle-client`) сам скачивает Basic-пакет (без SQL*Plus)
Instant Client 21.13 с прямого login-free CDN Oracle:

```
https://download.oracle.com/otn_software/linux/instantclient/2113000/instantclient-basic-linux.x64-21.13.0.0.0dbru.zip
```

Чтобы встроить клиент в образ l2-worker:

```
L2_WORKER_DOCKER_TARGET=runtime-db docker compose up -d --build l2-worker
```

вместе с profile `oracle` и `DB_ORACLE_ENABLED=true`. По умолчанию сборка идёт с `target=runtime`
и клиент **не** кладётся в образ — так локальные/тестовые сборки с PostgreSQL не качают ~280 МБ.

Куда в контейнере: архив распаковывается в `/opt/oracle`, каталог `/opt/oracle/instantclient_21_13`.
Stage `runtime-db` регистрирует его в динамическом загрузчике:

- `/etc/ld.so.conf.d/oracle-instantclient.conf` → путь `/opt/oracle/instantclient_21_13`
- ставит пакеты `libaio1t64`, `libnsl2`
- создаёт compat-symlink `libaio.so.1` → `libaio.so.1t64` (Ubuntu t64-переход)

### Вариант 2 — вручную (offline, нет доступа к интернету)

Все действия можно выполнить без интернета, если заранее (на машине с сетью) подготовить три вещи:
**zip Instant Client**, **`.deb` для `libaio`** и (при необходимости) `unzip`. Подробно про ld.so
регистрацию — в отдельном разделе ниже.

**Важно про текущий runtime-образ l2-worker (сборка `target=runtime`):** в нём **нет** `unzip`,
`dpkg` и `apt` (урезанный образ), зато `ldconfig` есть. Поэтому `.deb` распаковываются **на хосте**
(`dpkg -x`), а не ставятся внутри контейнера, и zip тоже распаковывается на хосте.

1. Скачайте **Basic Package (ZIP)** с официальной страницы Instant Client for Linux x86-64
   (на машине с интернетом): <https://www.oracle.com/database/technologies/instant-client/linux-x86-64-downloads.html>
   Версия, совпадающая с Dockerfile: `instantclient-basic-linux.x64-21.13.0.0.0dbru.zip`.

2. Распакуйте zip на хосте и занесите каталог в контейнер:

   ```bash
   mkdir -p /tmp/ic-src && cd /tmp/ic-src
   unzip /path/to/instantclient-basic-linux.x64-<ver>.zip    # → instantclient_21_13/
   docker cp /tmp/ic-src/instantclient_21_13 l2-worker:/opt/oracle/instantclient_<ver>
   ```

   (Каталог внутри должен называться так же, как распаковалось из zip — обычно `instantclient_21_13`.)

3. Доставьте `libaio`. Нужен только `.deb` (на Ubuntu 26.04/24.04 amd64 это `libaio1t64`), который
   содержит настоящую библиотеку `libaio.so.1t64`. `libnsl.so.1` в runtime-образе **уже есть**
   (проверено), отдельный `libnsl2` ставить не нужно. Распакуйте `.deb` на хосте:

   ```bash
   dpkg -x /path/to/libaio1t64_*.deb /tmp/libaio-root
   # внутри появится usr/lib/x86_64-linux-gnu/libaio.so.1t64
   docker cp /tmp/libaio-root/usr/lib/x86_64-linux-gnu/libaio.so.1t64 \
     l2-worker:/usr/lib/x86_64-linux-gnu/
   ```

4. Соберите `libaio.so.1` (SONAME, который ищет `libclntsh.so.21.1`) и зарегистрируйте каталог
   клиента в загрузчике:

   ```bash
   docker compose exec l2-worker bash -c \
     'ln -sf /usr/lib/x86_64-linux-gnu/libaio.so.1t64 /usr/lib/x86_64-linux-gnu/libaio.so.1 && \
      echo /opt/oracle/instantclient_<ver> > /etc/ld.so.conf.d/oracle-instantclient.conf && \
      ldconfig'
   ```

5. Перезапустите l2-worker и проверьте, что ODPI-C нашёл `libclntsh.so`:

   ```bash
   docker compose restart l2-worker
   docker compose logs l2-worker | grep -i oracle   # ждать "DB executor 'oracle': pool ready"
   ```

Примечания:

- Версия Instant Client не обязана совпадать с версией СУБД — OCI-клиент 21.x работает и с Oracle XE 21c.
- Имя каталога зависит от версии (`instantclient_21_13`, `instantclient_21_23`, ...) — используйте тот, что распаковался.
- SQL*Plus/Tools/SDK не нужны — шлюзу достаточно Basic-пакета.
- Если используете `tnsnames.ora`/`sqlnet.ora`, положите их в `network/admin` внутри каталога клиента (или задайте `TNS_ADMIN`); пулу шлюза это не нужно (он ходит по host:port/service).
- Изменения, сделанные командами в **живом** контейнере, **эфемерны** — исчезают при `docker compose up`/recreate. Для постоянной offline-инсталляции собирайте образ (см. «Офлайн-сборка образа» ниже).

## ld.so регистрация подробно (offline)

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
  `libclntsh.so` — это symlink, который уже лежит внутри Basic-zip; ODPI-C перебирает имена
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

### Почему правки в живом контейнере эфемерны → офлайн-сборка образа

`docker compose` (re)create контейнера пересобирает его из образа, и все ручные изменения
(`ld.so.conf.d`, симлинки, сам каталог `/opt/oracle`) теряются. Для постоянной установки лучше
положить клиент в образ при сборке. Офлайн-вариант: держать zip и `.deb` в контексте сборки и
заменить онлайн-скачивание на `COPY` + `unzip`/`dpkg-deb -x` (без `apt-get` из интернета):

```dockerfile
# docker build offline: кладём рядом с Dockerfile:
#   offline/instantclient-basic-linux.x64-21.13.0.0.0dbru.zip
#   offline/libaio1t64_*.deb
# (unzip может понадобиться и в runtime-образе, либо распакуйте zip заранее и COPY каталогом)
FROM ubuntu-base AS oracle-client-offline
COPY offline/instantclient-basic-linux.x64-21.13.0.0.0dbru.zip /tmp/ic.zip
COPY offline/*.deb /tmp/debs/
RUN set -eux; \
    apt-get update && apt-get install -y --no-install-recommends unzip libnsl2 && \
    dpkg -i /tmp/debs/*.deb && \
    unzip -q /tmp/ic.zip -d /opt/oracle && \
    rm -rf /tmp/ic.zip /tmp/debs /var/lib/apt/lists/*

# далее — как в штатном stage runtime-db: COPY --from=oracle-client-offline /opt/oracle /opt/oracle
# + /etc/ld.so.conf.d/oracle-instantclient.conf + ldconfig + symlink libaio.so.1
```

В штатном `cpp/l2-proxy/Dockerfile` эти шаги выполняют stage `oracle-client` (онлайн-скачивание
zip, строки с `curl ... otn_software ...`) и `runtime-db` (`apt-get install libaio1t64 libnsl2`).
Для офлайн-сборки замените `curl`-шаг на `COPY offline/*.zip` + `unzip`, а `apt-get install
libaio1t64` — на `dpkg -i <libaio1t64.deb>`. Проще всего собрать и выгрузить образ один раз на
«онлайн-машине» (`docker save`/`docker load`), если контур разворачивается на offline-хосте без
прав на сборку.

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

### 5. Ошибки

- `404 UNKNOWN_DATABASE` — неизвестная база (`/v1/sql/mssql/query`)
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
- Поддерживаемые драйверы: PostgreSQL (libpq, включён по умолчанию) и Oracle (ODPI-C / Instant Client 21.13, опционально через profile `oracle`).

# HTTP DB Gateway — как это работает (пример)

Полный контур: **HTTP → l2-proxy → NATS (`service.db.query`) → l2-worker → Oracle (ODPI-C pool) → обратно**.

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
| `oracle` | БД | `gvenzl/oracle-xe:21.3.0-slim`, демо-схема из `sql/init/init.sql`. Запускается только через profile: `docker compose --profile oracle up -d` |
| `l2-proxy` | HTTP-вход | регистрирует БД из `DB_ORACLE_*` env, роутит `/v1/sql/*` в NATS |
| `l2-worker` | Исполнитель | образ `runtime-db` (Oracle Instant Client 21.13), держит пул сессий, отвечает на `service.db.query` |
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

## Демо-данные

`sql/init/init.sql` создаёт таблицу `app_user.demo_messages` (id/message/created_at) и заполняет её:

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
{"databases":[{"driver":"oracle","enabled":true,"name":"oracle"}]}
```

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
- Драйвер — Oracle (ODPI-C / Instant Client 21.13), другие БД пока не подключены.

# Раунд 265: обновление вендорных либ (nats.c 3.15.0, cpp-httplib 0.67.0, odpi 26.1.0)

## Цель

Поднять вендорные библиотеки до свежих апстримных тегов: nats.c, cpp-httplib, odpi.

## Что сделано

### 1. Обновление пинов в `scripts/update-vendored-libs.py`

| Библиотека | Было | Стало |
|---|---|---|
| nats.c | v3.14.0 | v3.15.0 |
| cpp-httplib | v0.63.0 | v0.67.0 |
| odpi | v26.0.0 | v26.1.0 |

### 2. Синхронизация деревьев либ

- **nats.c v3.15.0**: `src/nats/src/` обновлён по апстриму. В апстриме переработана
  обработка TLS/URL (`tls://`-схема, авто-включение secure, `tlsScheme` в `natsUrl`,
  отказ от ws/wss), упорядоченные consumer-ы JetStream (`jsSub_resetOrderedConsumer`
  без sseq, `_setOrderedConsResumeSeq`, имя consumer `<Name>_<n>` при рекриейте),
  серверный пул (user info из server-supplied URL отбрасывается, entry без порта),
  версия `3.15.0` (не beta). Локальные правки отладки (лог dialing IP:port, TLS
  handshake/timeout из раундов 243/254) апстримом перезаписаны — решено оставить
  чистый апстрим, т.к. свежая версия уже содержит переработанную TLS-логику.
  `src/nats/CMakeLists.txt` (локальный патч: examples/test за опциями) сохранён.
- **cpp-httplib v0.67.0**: `src/httplib/httplib.h` + `httplib.cc` пересобраны из
  single-header штатным `split.py` тега v0.67.0. В апстриме: SSEParser, доработки
  stop()/reconnect, websocket-доработки. Удалены артефакты `httplib.cc.bak-063` /
  `httplib.h.bak-063` (бэкапы старого v0.63.0).
- **odpi v26.1.0**: `src/odpi/` обновлён (dpiStmt re-execute, версия 26.1.0,
  `DPI_VERSION_SUFFIX` пуст).
- **nlohmann-json v3.12.0**: пин не менялся, но `json.hpp`/`json_view.hpp`
  синхронизированы с апстримом v3.12.0 (`JSON_RECURSION_DEPTH_LIMIT`,
  `JSON_UBJSON_VALUELESS_ELEMENTS_PER_BYTE`, char8_t, vector-оптимизации).

### 3. VENDORED-LIBS.md

Таблица обновлена: nats.c 3.15.0, cpp-httplib 0.67.0, odpi 26.1.0.

## Важные решения

- Локальные отладочные fprintf в nats.c (comsock.c/conn.c) не восстанавливаются —
  чистый апстрим v3.15.0 уже переработал TLS/URL-логику, старые правки устарели.
- Локальный патч `nats/CMakeLists.txt` (examples/test за опциями) сохранён.
- `httplib.cc.bak-063`/`httplib.h.bak-063` — бэкапы старого тега, удалены.

## Проверка

Пересборка через `./rebuild-and-run.sh` + `python3 message_counter.py
--iterations 1 --concurrent 1` + `health-check.sh all` — все rc=0.

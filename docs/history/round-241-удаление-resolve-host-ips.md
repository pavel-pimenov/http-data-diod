# Round 241 — Удалена resolve_host_ips (DNS-резолв не нужен для диагностики)

## Задача
Проблема NATS connect (Timeout) была в конфиге nats-server (однострочный
tls-блок, исправлен в round-240), а не в DNS-резолве. Функция
`resolve_host_ips()` (getaddrinfo, логирование `[resolves to: IP]`) — лишняя,
не нужна для диагностики.

## Реализация
### `src/nats_client.cpp`
- Удалена функция `resolve_host_ips()` и её includes
  (`<arpa/inet.h>`, `<netdb.h>`, `<sys/socket.h>`).
- Убран вызов в `connect()`: лог `Waiting for NATS server... [resolves to: ...]`
  → `Waiting for NATS server...`.
- Убран вызов в ошибке connect: `(resolves to: ...)`.
- Оставлена диагностика `(dialed: nats://...:4222)` через
  `natsConnection_GetConnectedUrl` — она полезна (показывает фактический URL
  из opts).

## Проверка
- Dockerfile.test: сборка зелёная, test_components 3099/632, test_proxy_core
  952/126.
- rebuild-and-run.sh зелёный, все сервисы healthy.
- Логи: `Waiting for NATS server...` (без resolves), `Connected to NATS server:
  nats://nats-server:4222 (dialed: nats://nats-server:4222)`.
- message_counter exit 0.

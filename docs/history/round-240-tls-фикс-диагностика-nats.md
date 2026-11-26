# Round 240 — Фикс TLS-конфига nats-server + диагностика NATS connect (URL/dialed/DNS)

## Задача
1. Включение `NATS_ENABLE_TLS=true` (самоподписанные серты в certs_nats/)
   ломало nats-server: `Parse error on line 16: Expected a map value terminator,
   got 'k'`.
2. Нужна диагностика NATS connect: какой URL реально используется в опциях и
   куда резолвится hostname (чтобы видеть, если коннект идёт не туда).

## Причина (проблема 1)
В `docker-compose.yml` tls-блок генерировался однострочным
`printf 'tls { cert_file: "..." key_file: "..." }'`. NATS config **не
поддерживает** несколько полей на одной строке: `cert_file: "..." key_file: ...`
— nats-server парсит `cert_file`, потом натыкается на `key_file` (буква `k`) →
"got 'k'". Плюс `printf '...\n'` — `\n` терялся в YAML-экранировании, строка
склеивалась.

## Реализация
### `docker-compose.yml` (nats-server command)
- tls-блок переписан на многострочный через `echo` (каждое поле на своей строке):
  ```
  tls {
    cert_file: "/etc/nats/certs_nats/server.crt"
    key_file: "/etc/nats/certs_nats/server.key"
  }
  ```
- authorization-блоки (token / user+password) тоже переведены с `printf '...\n'`
  на `echo` (надёжнее, `\n` не теряется в YAML).

### `src/nats_client.cpp` (диагностика)
- После успешного connect: `natsConnection_GetConnectedUrl` — фактический URL,
  который реально продиален клиентом (из opts), логируется как
  `(dialed: nats://...:4222)` — видно, если он отличается от ожидаемого.
- `resolve_host_ips()` добавлена, но в round-241 удалена (проблема была в
  конфиге, DNS-резолв не нужен) — см. round-241.

## Проверка
- Самоподписанные сертификаты сгенерированы в `certs_nats/` (ca.crt, ca.key,
  server.crt, server.key, SAN: nats-server, localhost, 127.0.0.1, 10.0.0.0).
- `.env` (не трекается): `NATS_ENABLE_TLS=true`,
  `NATS_TLS_CA_CERT_FILE=/etc/nats/certs_nats/ca.crt`.
- rebuild-and-run.sh зелёный, nats-server: `TLS required for client connections`,
  все сервисы healthy.
- l2-proxy: `Connected to NATS server: nats://nats-server:4222 (dialed:
  nats://nats-server:4222)` — TLS-связка работает.
- l2-worker: `Connected to NATS server: nats://nats-server:4222 (dialed:
  nats://nats-server:4222)`.
- message_counter exit 0 (no message loss).
- `docker build --check` валиден.

## Ключевое
NATS config не поддерживает несколько полей на одной строке в `tls {}` — только
многострочный формат. Диагностика теперь показывает реальный dialed URL и
DNS-резолв hostname.

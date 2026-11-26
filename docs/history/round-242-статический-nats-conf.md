# Round 242 — Статический nats.conf (TLS в файле) вместо генерации в скрипте

## Задача
Убрать генерацию конфига nats-server из shell-скрипта в docker-compose и
заменить на нормальный статический файл `nats/nats.conf`, закоммиченный в
репозиторий и смонтированный. TLS-пути фиксированы, auth — через env (секреты
не коммитим).

## Реализация
### `nats/nats.conf`
- Добавлен статический TLS-блок (пути `/etc/nats/certs_nats/server.{crt,key}`):
  ```
  tls {
    cert_file: "/etc/nats/certs_nats/server.crt"
    key_file: "/etc/nats/certs_nats/server.key"
  }
  ```
- База (port, http_port, jetstream, max_payload) осталась.
- Комментарий: TLS всегда включён; чтобы отключить — закомментировать блок.

### `docker-compose.yml` (nats-server command)
- Убрана генерация TLS-блока из скрипта (он теперь в статическом nats.conf).
- Скрипт оставляет только **auth через env**: если задан `NATS_TOKEN` или
  `NATS_USER`/`NATS_PASSWORD`, копирует nats.conf (с TLS) в /tmp и добавляет
  authorization-блок — секрет не попадает в cmdline и не коммитится.
- `NATS_ENABLE_TLS` env для nats-server больше не используется в command
  (TLS всегда в статическом файле), но остаётся для клиентов.

## Проверка
- `docker compose up -d nats-server`: nats-server `TLS required for client
  connections`, Server is ready (конфиг валиден).
- rebuild-and-run.sh зелёный, все сервисы healthy.
- l2-proxy: `Connected to NATS server: nats://nats-server:4222 (dialed:
  nats://nats-server:4222)`.
- message_counter exit 0 (no message loss).

## Ключевое
TLS теперь в закоммиченном статическом `nats/nats.conf` (многострочный блок,
валидный для NATS). В скрипте осталась только опциональная генерация auth
(секреты через env, не в git).

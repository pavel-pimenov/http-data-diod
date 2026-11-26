# Round 261 — Версия 2.0.8 + фикс connect-timeout NATS (мс vs сек) — корневая причина

## Задача
Разобраться, почему NATS-воркер на проде не может подключиться к
`k2-datahub-dev.k2.ia.nlmk:4222` (TLS-хендшейк проходит, но connect уходит в
Timeout), хотя:
- `nats request` с клиента работает;
- в локальном docker-compose (всё в одном контейнере) всё работает.

## Диагностика (шаги)
1. Конфиг NATS-сервера (`k2-datahub-dev`): TLS `ca_file` (НЕ mTLS — сервер не
   требует client cert), `timeout: 2` на TLS-хендшейк.
2. `openssl s_client` с воркера: `Verify return code: 0 (ok)` (CA валиден),
   но `Cipher is (NONE)` + `packet length too long` — потому что NATS требует
   INFO-протокол до TLS, openssl s_client не подходит для проверки.
3. `curl` на `172.23.201.11:4222`: NATS 2.14.1 отвечает INFO с
   `"tls_required":true`.
4. `tcpdump` с воркера: воркер шлёт ClientHello (1876 байт, 3 TCP-сегмента) и
   **сам сбрасывает RESET**, не дождавшись ServerHello.
5. Лог воркера (2.0.7): `[nats] TLS negotiated: TLSv1.3 TLS_AES_128_GCM_SHA256`
   — **TLS-хендшейк успешен**, но connect мгновенно уходит в `Timeout` (та же
   миллисекунда, что и TLS negotiated).
6. Анализ nats.c: `natsOptions_SetTimeout` документирует **миллисекунды**
   (nats.h:3268, дефолт `2*1000` = 2000 мс), но воркер передавал
   `ms_to_seconds(5000)` = **5** (секунды) → `opts->timeout = 5 мс` →
   deadline ≈ 5 мс → практически мгновенный Timeout.
7. В проде сеть между VM (~6 мс) чуть больше 5 мс → Timeout. В локальном
   docker-compose connect мгновенный (< 5 мс) → успевает. Поэтому "в одном
   контейнере работает".

## Корневая причина
Воркер (`nats_client.cpp`) передавал `natsOptions_SetTimeout` значение в
**секундах** (5) вместо **миллисекунд** (5000) через `ms_to_seconds()`.
nats.c ожидает миллисекунды. Не nats.c-либа, не TLS, не CA, не сеть, не сервер.

## Реализация
- `src/nats_client.cpp:331`: `natsOptions_SetTimeout(opts, m_config.m_connect_timeout_ms)`
  — передавать мс напрямую (убрать `ms_to_seconds`).
- `src/nats_client.cpp:823`: `natsConnection_DrainTimeout(conn, timeout_ms)` —
  передавать мс напрямую (убрать `ms_to_seconds`).
- `src/nats_client.hpp:262`: удалён неиспользуемый `ms_to_seconds`.
- `src/generate_version.sh`: `2.0.7-$SHA` → `2.0.8-$SHA`.

## Проверка
- `./rebuild-and-run.sh` — сборка зелёная, health checks passed.
- Локально: `Connected to NATS server: nats://nats-server:4222 (dialed: ...)`.

## Связь с прод-архивом (/tmp/ks)
- `k2-datahub-dev` = `172.23.201.11` (inventory.ini) — VM с NATS + прокси + nginx.
- `k2l25-services-dev` = `172.24.138.55` — VM воркера.
- Прод-конфиг использует самоподписанные серты (server-cert/key, ca-cert.pem).
  TLS и CA работали корректно (TLS negotiated успешен); проблема была именно в
  connect-timeout, не в сертификатах.

## Тэг
- `git tag -f 2.0.8 HEAD && git push origin 2.0.8` — тэг для CI-сборки.

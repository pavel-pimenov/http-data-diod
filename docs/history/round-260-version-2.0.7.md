# Round 260 — Версия 2.0.7 + логирование TLS-версии/шифра + openssl-диагностика

## Задача
Поднять версию до `2.0.7`, добавить логирование согласованных TLS-версии и
шифра в NATS C-клиент, чтобы сравнить с nats.go CLI и понять, почему сервер
рвёт TLS-хендшейк воркера (`connection reset by peer`).

## Реализация
- `src/generate_version.sh`: `VERSION "2.0.6-$SHA"` → `VERSION "2.0.7-$SHA"`.
- `src/nats/src/conn.c` (после успешного TLS-хендшейка): логируется
  `[nats] TLS negotiated: host=... version=... cipher=...` через
  `SSL_get_version` + `SSL_get_cipher_name`.
- Диагностика с хоста воркера (`openssl s_client`):
  ```bash
  openssl s_client -connect k2-datahub-dev.k2.ia.nlmk:4222 \
    -CAfile /etc/nats/certs/ca-cert.pem \
    -servername k2-datahub-dev.k2.ia.nlmk </dev/null 2>&1 | \
    grep -E "Verify return code|subject=|issuer=|Protocol|Cipher"
  ```

## Вывод из серверного лога
Сервер показывает `TLS handshake error: read: connection reset by peer` для
воркера (cid:69/71), но nats request (cid:70) проходит TLS 1.3. Разница — в
TLS-настройках/ClientHello воркера vs nats.go.

## Проверка
- `./rebuild-and-run.sh` — сборка зелёная; в логах l2-proxy:
  `[nats] TLS negotiated: host=nats-server version=TLSv1.3 cipher=TLS_AES_128_GCM_SHA256`.

## Тэг
- `git tag -f 2.0.7 HEAD && git push origin 2.0.7` — тэг для CI-сборки.

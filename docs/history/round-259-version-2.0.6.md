# Round 259 — Версия 2.0.6 + логирование типа auth NATS и TLS-хендшейка

## Задача
Поднять версию до `2.0.6`, добавить логирование типа подключения к NATS
(юзера+пароль / токен / creds-файл), чтобы в логе было понятно, какой
auth-механизм реально используется, и логировать детали TLS-хендшейка.

## Реализация
- `src/generate_version.sh`: `VERSION "2.0.5-$SHA"` → `VERSION "2.0.6-$SHA"`.
- `src/nats_client.cpp`:
  - конструктор логирует `auth_type=` (token / username/password /
    credentials_file) — какой механизм реально выбран;
  - `setup_options` логирует `NATS authentication type: <тип>` на info-уровне
    (раньше debug — скрыт при LOG_LEVEL=INFO), с user и признаком password
    не задан.
- `src/nats/src/conn.c` (TLS-хендшейк):
  - при ошибке `SSL_do_handshake` — `[nats] TLS handshake failed: ssl_err=... errno=... detail=<verify/CA/SNI>`;
  - при таймауте ожидания сервера (WANT_READ/WANT_WRITE) —
    `[nats] TLS handshake timed out waiting for <server response/socket> (host=...)`.

## Проверка
- `./rebuild-and-run.sh` — сборка зелёная, все сервисы healthy.

## Тэг
- `git tag -f 2.0.6 HEAD && git push origin 2.0.6` — тэг для CI-сборки.

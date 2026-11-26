# Round 257 — Диагностика TLS-конфига NATS (пустой CA / неполная пара cert+key)

## Задача
Если `NATS_ENABLE_TLS=true`, но клиентский TLS-файл не задан/неполон —
ругаться в лог подробно, что именно забыто, вместо молчаливого пропуска.

## Реализация
`src/nats_client.cpp` (`setup_options`, TLS-блок):
- если TLS включён, но `NATS_TLS_CA_CERT_FILE` пуст — раньше блок
  `if (!m_tls_ca_cert_file.empty())` просто пропускался и клиент стартовал
  TLS без trust-якоря, виснув на хендшейке; теперь добавлена `else`-ветка,
  которая `Logger::error` + `set_error` с подробным сообщением:
  какой env-переменной нет, куда указать (`/etc/nats/certs_nats/ca.crt`),
  и что без неё хендшейк не завершится; возвращается `false`;
- если задан только один из пары `NATS_TLS_CERT_FILE`/`NATS_TLS_KEY_FILE` —
  раньше блок `if (cert && key)` пропускался молча; теперь `else if` ругается,
  какая из переменных не задана, а какая задана; возвращается `false`.

## Проверка
- `./rebuild-and-run.sh` — сборка зелёная, все сервисы healthy.
- В логах l2-proxy видны info-строки:
  `NATS TLS CA cert file: /etc/nats/certs_nats/ca.crt` (путь из окружения) и
  `NATS CA certificate loaded: /etc/nats/certs_nats/ca.crt` (реальная загрузка).

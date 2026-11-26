# Round 256 — Версия 2.0.4 + логирование путей TLS-сертов NATS и причины connect

## Задача
Поднять версию до `2.0.4`, добавить логирование того, по каким путям
читаются TLS-сертификаты NATS (чтобы видеть, видит ли сервис конфиг),
и логировать первопричину неудачного connect (TLS/IO ошибку).

## Реализация
- `src/generate_version.sh`: `VERSION "2.0.3-$SHA"` → `VERSION "2.0.4-$SHA"`.
- `src/config.cpp`: при `NATS_ENABLE_TLS=true` после `NATS TLS: enabled`
  логируются фактические пути `NATS_TLS_CA_CERT_FILE`, `NATS_TLS_CERT_FILE`,
  `NATS_TLS_KEY_FILE` (или `(not set)`) — видно, какие пути реально
  прочитались из окружения.
- `src/nats_client.cpp`:
  - логи загрузки CA-серта и client-цепочки переведены с `Logger::debug`
    на `Logger::info` (иначе при `LOG_LEVEL=INFO` их не видно);
  - при неудачном `natsConnection_Connect()` читается
    `natsConnection_ReadLastError()` — в лог попадает первопричина
    (TLS-хендшейк/IO), которую библиотека прятала за generic-статусом;
  - `on_async_nats_error` тоже пробрасывает `ReadLastError` с connection.

## Проверка
- `./rebuild-and-run.sh` — сборка зелёная, все сервисы healthy,
  health checks passed.

## Тэг
- `git tag -f 2.0.4 HEAD && git push origin 2.0.4` — тэг для CI-сборки.

# Round 255 — Версия приложения поднята до 2.0.3 + логирование загрузки CA-серта

## Задача
Поднять версию приложения с `2.0.2` на `2.0.3` и добавить debug-логирование
загрузки TLS-сертификатов NATS (CA + client chain), чтобы видеть в логах,
что именно загружается.

## Реализация
- `src/generate_version.sh`: `VERSION "2.0.2-$SHA"` → `VERSION "2.0.3-$SHA"`.
- `src/l2-proxy-version.h` — build-generated (не трекается), пересоздаётся
  `generate_version.sh` при сборке, поэтому отдельно не менялся.
- `src/nats_client.cpp`: после успешной загрузки CA-серта
  (`natsOptions_LoadCATrustedCertificates`) добавлен
  `Logger::debug("NATS CA certificate loaded: {}")`, после загрузки
  client-цепочки (`natsOptions_LoadCertificatesChain`) —
  `Logger::debug("NATS client certificate chain loaded: cert={}, key={}")`.
  Ошибки (файл не найден/нечитаем или NATS-функция вернула статус) уже
  логировались ранее через `check_file_readable` + `CHECK_NATS_OK`.

## Проверка
- `./rebuild-and-run.sh` — сборка зелёная, все сервисы healthy,
  health checks passed.
- `message_counter.py --iterations 1 --concurrent 1` — exit 0.

## Тэг
- `git tag -f 2.0.3 HEAD && git push origin 2.0.3` — тэг для CI-сборки.

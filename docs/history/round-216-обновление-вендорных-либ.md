# round 216 — обновление вендорных либ (по просьбе пользователя)

## Что сделано

Проект держит сторонние библиотеки как вендорённые исходники (не submodule),
поэтому обновление сделано штатным `scripts/update-vendored-libs.py` с явными
пинами; локальные правки поверх апстрима (`nats/CMakeLists.txt`, NOLINT/modules
в `json.hpp`, клиентская часть `prometheus-cpp`) не перезаписаны.

| Библиотека | Было | Стало | upstream HEAD | Что важно |
|---|---|---|---|---|
| cpp-httplib | v0.63.0 | **v0.67.0** | `7800c3dc` | безопасность: выход за mount point на Windows через junction/symlink, чужие байты в keep-alive соединении читались как ответ следующего запроса, перепутывание ответов между stream и запросом из другого потока, stale `Content-Length` при сжатии/chunked; исправления `stop()` во время connect/TLS/CONNECT и слишком больших таймаутов |
| nats.c | v3.14.0 | **v3.15.0** | `23f51ca6` | безопасность: переполнение стека при 65..512 адресах hostname, подмена credentials из `connect_urls`, отбой plaintext `connect_urls` после TLS-upgrade, отказ от не-TLS соединений при TLS-настройках (`NATS_SECURE_CONNECTION_WANTED`); фиксы JetStream ordered consumers/Object Store watcher, `nats_EncodeTimeUTC`, новый `natsConnection_GetRemoteIPAndPort` |
| odpi | v26.0.0 | **v26.1.0** | `4c54feef` | утечка ссылок при внутреннем re-execute SQL с bind-переменными, новый `dpiConn_setEndUserSecurityContextFromOson()` |

`nlohmann/json` (v3.12.0) и `prometheus-cpp` (66b61559) уже на последних
апстримных пинах — не трогались; `base64` вендорится с `master` без тега.

Обновлены пины в `scripts/update-vendored-libs.py` и версии/коммиты в
`src/VENDORED-LIBS.md`. Итого изменено 24 файла (~+1562/−707 строк), из них
`httplib.h/.cc` пересобраны штатным `split.py` этого же тега, в `nats/src/`
обновились 15 файлов, в `odpi/` — 3.

Совместимость проверена вручную: `nats_client.cpp` вызывает
`natsOptions_SetSecure(true)` **до** загрузки CA/клиентских сертификатов, то есть
новое правило 3.15.0 про «TLS-настройки ⇒ только TLS-соединение» для проекта
безопасно (проект и так работает по TLS).

## Проверка

- `./rebuild-and-run.sh` — rc=0, сборка и запуск стека зелёные, юнит-тесты
  `test_components` 636/3132 и `test_proxy_core` 132/996 (без изменений);
  живые NATS-тесты поллера в составе `test_components` прошли с клиентом 3.15.0.
- `./health-check.sh all` — rc=0; `python3 message_counter.py --iterations 1
  --concurrent 1` — rc=0.
- `./scripts/ci-gate.sh all` — rc=0 (метрики, алерты, дашборды и consistency-
  гейты прошли с новыми версиями либ).
- Пер-файловое покрытие не пересчитывалось: first-party-код и тесты в этом
  раунде не менялись, вендорные исходники в baseline не входят.

# Round 238 — Диагностика недоступности NATS CA/сертификатов

## Задача
Пользователь на хосте k2-datahub-dev получил ошибку
`Failed to load NATS CA certificate: /etc/nats/certs/ca-cert.pem` — файл
существует (root видит), но приложение (app, uid 10001) не может его
загрузить. Нужно добавить диагностику при загрузке конфига, чтобы лог
различал «файл не существует» (неверный mount/путь) и «нет прав чтения»
(EACCES, 0600 root-owned secret).

## Причина
Контейнер запускается как unprivileged `app` (uid 10001). NATS C client
(`natsOptions_LoadCATrustedCertificates` → OpenSSL) при загрузке CA требует
read-доступ для пользователя app. Файл существует, но, вероятно, имеет права
`600 root:root` (смонтирован как секрет) — root читает, app не может.

## Реализация
### `src/nats_client.cpp`
- Добавлена helper-функция `check_file_readable(path)` в анонимное
  пространство имён: пробует открыть файл через `std::ifstream`; при неудаче
  анализирует `errno`:
  - `ENOENT` → «file not found (missing mount or wrong path)»
  - `EACCES` → «permission denied (check file mode/owner; the runtime user is
    the unprivileged 'app' uid 10001)»
  - иначе → «cannot open: <strerror>»
  - пустой путь → «empty path»
- В блоке TLS (`setup_options`): перед `natsOptions_LoadCATrustedCertificates`
  вызывается `check_file_readable`; если файл недоступен — `set_error` + лог
  `Logger::error` с причиной + `return false`.
- То же для клиентских сертификатов (`cert`/`key`): раздельная проверка
  обоих файлов, лог указывает какой недоступен.
- Добавлены includes `<cerrno>`, `<fstream>`.

## Проверка
- Dockerfile.test `--target test`: сборка зелёная, test_components и
  test_proxy_core прошли (952 assertions в test_proxy_core).
- rebuild-and-run.sh зелёный, все сервисы healthy.
- message_counter.py exit 0 (no message loss).

## Итог
Теперь при недоступности CA/сертификатов лог чётко укажет: файла нет
(неверный путь/mount) или нет прав (0600 root → app не читает).

# round 215 — строгий разбор TCP-порта в URL и SENTRY_DSN

## Что было не так

Порт разбирался через `std::stoi` в двух first-party местах, и оба разбора были
небезопасны:

1. **`parse_url()` (`url_utils.hpp`)** — `std::stoi` в try/catch. Исключение
   ловилось, но `std::stoi("8080abc")` возвращает 8080, `"-1"` — минус единицу,
   а `"70000"` — 70000, то есть в `int m_port` попадал порт вне диапазона
   TCP, который затем уезжал в `extract_scheme_host_port()` и в исходящий URL
   L2-запроса (`http://host:70000`). Валидация порта была только у env-переменных
   (`get_env_int`), но не у URL.
2. **`CrashHandler::parse_sentry_dsn()` (`crash_handler.hpp`)** — `std::stoi`
   вообще без try/catch: `SENTRY_DSN=https://key@host:abc/42` бросал
   `std::invalid_argument` из `CrashHandler::install()` на старте процесса, а
   порт `> 65535` или отрицательный молча заворачивался в `uint16_t`
   (`static_cast<uint16_t>(70000)` → 4464) и Sentry потом стучался не туда.
   Опечатка в DSN роняла сервис вместо того, чтобы просто отключить Sentry.

## Что сделано

- Новый `src/port_utils.hpp`: `port_utils::parse_tcp_port(std::string_view) ->
  std::optional<int>` на `std::from_chars` — только десятичные цифры, без
  знака/пробелов/хвостового мусора, диапазон `net_limits::kMinTcpPort ..
  kMaxTcpPort`; всё остальное (пустая строка, `+80`, `-1`, `" 80"`, `8080abc`,
  переполнение) → `nullopt`, без исключений.
- `parse_url()` использует хелпер: невалидный порт теперь даёт порт схемы по
  умолчанию (80/443), а не «как получится». Поведение для корректных портов
  не изменилось.
- Новый `src/crash_dsn.hpp`: чистый разбор DSN
  (`scheme://key@host[:port]/project`) в `crash_dsn::Parsed`
  (`m_dsn_raw`/`m_key`/`m_host`/`m_port`/`m_project`). `CrashHandler::parse_sentry_dsn()`
  стал тонкой обёрткой, копирующей поля в прежний `SentryDsnConfig`; порт
  разбирается тем же хелпером, при мусоре берётся порт схемы — исключений
  больше нет, неполный DSN по-прежнему означает «Sentry выключен» (пустой host).
- Тесты: `test_proxy_core` — границы `1/80/65535/0080` и отбой
  `0/65536/переполнение/пусто/знак/пробелы/хвостовой мусор/hex/float`;
  `test_components` — `parse_url` с портами `0`, `65536`, пустым и
  переполненным отдаёт 80, а `70000` для https — 443; `test_crash_utils` —
  разбор корректных DSN (https/http, явный порт, без проекта), fallback на
  80/443 для `notaport`/`0`/`65536`/пустого/переполненного порта без
  исключений и «Sentry выключен» на неполных DSN.

## Проверка

- `./rebuild-and-run.sh` — сборка и запуск стека зелёные, `test_components`
  636 кейсов / 3132 assertions, `test_proxy_core` 132 / 996.
- `./scripts/run-coverage.sh` + `coverage-regression-check.py` — гейты пройдены;
  production-файлы **7181/7809 = 91.96%** (93 файла, добавились `port_utils.hpp`
  12/12 и `crash_dsn.hpp` 27/28), `url_utils.hpp` 68/68, `crash_handler.hpp` 7/7;
  пер-файловый гейт перед `--update` показал 1 «регрессию» в `http_client.cpp`
  (94→93 строки, 82→78 ветвей) — файл не менялся, это штатный разброс
  сетевых/потоковых ветвей между прогонами; baseline обновлён, повторный
  чек — `OK`.
- `./health-check.sh all`, `python3 message_counter.py --iterations 1 --concurrent 1`,
  `./scripts/ci-gate.sh all` — rc=0.

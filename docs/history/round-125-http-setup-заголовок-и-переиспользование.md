# Round 125: вынос HTTP-setup из umbrella-заголовка и переиспользование в Sentry

## Что сделано

### 1. `setup_http_connection` / `setup_ssl_client` вынесены в `http_setup.hpp`

Хелперы настройки httplib-соединения (таймауты/keep-alive + SSL-верификация)
жили прямо в umbrella-заголовке `common_utils.hpp`, хотя это отдельная,
самодостаточная тема. Они вынесены в новый узкий заголовок
`src/http_setup.hpp` (шаблон `setup_http_connection` + объявление
`setup_ssl_client`; определение `setup_ssl_client` остаётся в
`common_utils.cpp`). `common_utils.hpp` теперь просто ре-экспортирует
`http_setup.hpp` — обратная совместимость для всех, кто включал umbrella,
сохранена. Это убирает запах «новый код должен включать umbrella ради одного
хелпера».

### 2. Убран дубль настройки таймаутов в `sentry_client.cpp`

`sentry_client.cpp` (`send_envelope`) в обеих ветках — `httplib::SSLClient` и
`httplib::Client` — вручную повторял один и тот же блок:

```cpp
client.set_connection_timeout(5, 0);
client.set_read_timeout(timeout_seconds, 0);
client.set_write_timeout(timeout_seconds, 0);
```

Это ровно то, что делает `setup_http_connection(client, timeout_seconds,
enable_connection_reuse=false)` (без keep-alive — Sentry-отправка
fire-and-forget). Оба блока заменены одним вызовом общего хелпера; поведение
идентично (в `httplib::ClientImpl` `keep_alive_`/`nodelay` по умолчанию
выключены, как и раньше).

## Изменения

* **`src/http_setup.hpp`** (новый) — объявление `setup_ssl_client` и шаблон
  `setup_http_connection` (перенесены из `common_utils.hpp`).
* **`src/common_utils.hpp`** — вместо перенесённого кода `#include
  "http_setup.hpp"` (ре-экспорт для обратной совместимости).
* **`src/common_utils.cpp`** — прямой include `http_setup.hpp` (там определение
  `setup_ssl_client`).
* **`src/http_client.cpp`** — прямой include `http_setup.hpp` (использует оба
  хелпера).
* **`src/sentry_client.cpp`** — include `http_setup.hpp`; дублированный блок
  таймаутов в двух ветках заменён на `setup_http_connection(..., false)`.

Публичные имена/поведение не меняются; `setup_http_connection` — по-прежнему
шаблон в заголовке (inline, без ODR-проблем).

## Проверка

* `./rebuild-and-run.sh`: rc=0, юнит-тесты `All tests passed (2816 assertions in 589 test cases)`.
* `./health-check.sh all`: rc=0.
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0.
* cppcheck по изменённым файлам — чисто; clang-tidy — без новых ошибок
  (include-cleaner-шум по umbrella-реэкспорту предсуществующий).
* coverage: новый `http_setup.hpp` — 8/8 строк (100%); `common_utils.hpp`
  30→22 строк (шаблон ушёл), `sentry_client.cpp` 232→228 (только удаление
  покрытых строк), производственных файлов 79→80; baseline переанкорен,
  общий production 91.38% (6520/7135).
* `scripts/metrics-consistency-check.py --offline` — ок (метрики не менялись).

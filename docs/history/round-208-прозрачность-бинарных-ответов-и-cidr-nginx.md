# round 208 — невалидный UTF-8 в заголовках ответа и CIDR для `/metrics`

## Что было не так

Проверка логов живого стека (round 203–207) дала два дефекта, оба видны только
по логам/метрикам — health-check их не ловит:

1. **`nginx /metrics` отдавал 403, метрики nginx не собирались.** `nginx-exporter`
   (`SCRAPE_URI=http://nginx/metrics`) получал `403 access forbidden by rule`
   каждые 10 с — 8640 error-строк за сутки. В `nginx.conf` стояло
   `allow 172.22.0.0/16`, а сеть `l2_network` живёт на `172.24.0.0/16`
   (`docker-compose.yml`). Последствие: `nginx_up 0`, серий `nginx_*` нет вообще
   (`count(nginx_connections_active)` — NO DATA), панели nginx в Perses пустые.
   Это третий заход на те же грабли: `172.20 → 172.22` чинилось коммитом
   `763aaff`, и подсеть уехала снова.
2. **Любой ответ-ошибка апстрима превращался в 500.** `GET /favicon.ico` в живой
   цепочке: PTOKAX отвечает `404`, воркер падал с
   `Error processing NATS request: [json.exception.type_error.316] invalid UTF-8 byte at index 1: 0xE1`,
   клиент получал `500` вместо `404`. Причина — **заголовок**, а не тело:
   на ошибках HTTP-слой PTOKAX отдаёт локализованный `Date` в **Windows-1251**
   (`Date: Сб, 31 окт 2026 …` → байты `D1 E1 …`), `HeaderUtils::headers_to_json()`
   клал значение в JSON как есть, и `dump()` конверта NATS бросал
   `type_error.316` (индекс 1 — это `0xE1`, вторая буква «Сб»). Успешные ответы
   хаба заголовка `Date` не содержат вовсе (`Content-Length` + `Content-Type`),
   поэтому 142 тыс. боевых запросов шли нормально, а падали только 404 —
   то есть любая ошибка апстрима маскировалась под 500.
   Та же дыра была у **запроса**: `filter_headers_to_json()` писал значения
   заголовков клиента в конверт без проверки UTF-8.
   Отдельно: `prepare_response_data()` определял `is_binary` только по
   `Content-Type`, поэтому текстовый ответ с бинарным телом (cp1251-страница
   ошибки) тоже ронял сериализацию.

Контекст, не дефект: `message_counter.py` в live-режиме падает **по
определению** (round 203, раздел «Ограничения»): POST ждёт корреляционное эхо
`l2-server`, GET ждёт `200` + ICO, а живой PTOKAX на `/favicon.ico` отвечает
`404`. Проверка живой цепочки — `scripts/live-chain-check.py`.

## Что сделано

### UTF-8: валидатор, санитайзер и оба конверта

- Новый `src/utf8_utils.hpp`:
  - `sequence_length()` — длина корректной последовательности UTF-8 или 0
    (отвергаются одиночные continuation-байты, обрезки, overlong-формы,
    UTF-16 surrogates, кодпоинты выше `U+10FFFF`);
  - `is_valid_utf8()` — проверка всей строки;
  - `sanitize_invalid()` — заменяет каждый невалидный байт на `U+FFFD`
    (`EF BF BD`), результат всегда сериализуется nlohmann.
- `HeaderUtils::headers_to_json()` и `HeaderUtils::filter_headers_to_json()`
  (`src/header_utils.hpp`) прогоняют значения через `sanitize_invalid()`:
  заголовки ответа апстрима и заголовки запроса клиента больше не могут
  уронить сериализацию конверта. Байты, которые уже валидны, не меняются.
- `L2Worker::prepare_response_data()` (`src/l2_worker.cpp`):
  `m_is_binary = is_binary_content_type(ct) || !is_valid_utf8(body)`, а
  `m_content_type` тоже санитизируется. Текстовый ответ с бинарным телом уходит
  в конверт base64 и возвращается байт-в-байт, `Content-Type` проверяется
  первым (скан тела — только для текстовых ответов).
- Тесты:
  - `src/test_l2_worker.cpp` — helper `prepare_response_data` принимает тело;
    кейсы `text/html` + невалидный UTF-8 → `is_binary`, `text/html; charset=utf-8`
    + HTML → не бинарь; отдельный `TEST_CASE` на валидатор (ascii, 2/3/4-байтные
    последовательности, одиночный `0xE1`, обрезка, overlong, surrogate,
    `> U+10FFFF`, `0xFF`) и на санитайзер (валидный текст не меняется,
    `D1 E1` → два `U+FFFD`).
  - `src/test_proxy_core.cpp` — `headers_to_json`/`filter_headers_to_json` с
    cp1251-`Date`: значение становится валидным UTF-8, `dump()` не бросает.
- `scripts/live-chain-check.py` — новый сценарий `404 /favicon.ico`: цепочка
  идёт через собственный nginx диода (`--diode-url`, по умолчанию
  `http://localhost:7777`), потому что nginx хаба перед test-port отвечает на
  `GET` `405 Only POST is allowed`. Без этого сценария регресс «404 → 500»
  снова прошёл бы мимо проверки: остальные пять сценариев — успешные ответы,
  а у них заголовка `Date` нет вообще.

### nginx: allow-list на весь приватный docker-диапазон

- `nginx.conf`, `location /metrics`: `allow 172.16.0.0/12` вместо конкретной
  `/16`. Docker выдаёт bridge-сети из `172.16.0.0/12`, поэтому смена подсети
  (`172.20 → 172.22 → 172.24`) больше не ослепляет nginx-метрики.

## Проверка

- **nginx-метрики**: `nginx_up 1` (было `0`), в VictoriaMetrics появились
  `nginx_connections_active` / `nginx_connections_accepted`; спам
  `403 access forbidden by rule` в логе nginx прекратился.
- **Ответ-ошибка больше не превращается в 500**: `GET /favicon.ico` через
  `nginx:7777` → `404`, тело 30 байт (`Error 404: Not Found\nNot Found`),
  `Content-Type: text/plain; charset=utf-8`; `type_error.316` в логе воркера
  за прогон — 0. `GET /fly-stat` — тоже `404` вместо `500`.
- **Прозрачность живой цепочки**: `scripts/live-chain-check.py` — **6/6
  сценариев** побайтово, включая новый `404 /favicon.ico` (было 5 сценариев,
  и ни один не покрывал ошибку апстрима).
- **Стенд**: `./health-check.sh all` — rc=0, все сервисы и наблюдаемость.
- **Сборка**: `COMPOSE_FILE=docker-compose.yml:docker-compose.live.yml
  ./rebuild-and-run.sh` — rc=0 (Perses 9/9).
- **C++ юнит-тесты**: `src/run_tests.sh` в образе `http-data-diod:builder`.

## Ограничения (честно)

- Невалидные байты в заголовках заменяются на `U+FFFD`, то есть такой заголовок
  доезжает до клиента не побайтово. Это компромисс: JSON-конверт NATS не может
  хранить произвольные байты, а `Date` в cp1251 всё равно не парсится по
  RFC 7231. Тела ответов при этом остаются побайтовыми (base64).
- `172.16.0.0/12` в allow-list означает, что `/metrics` доступен и с хоста
  через опубликованный порт 7777: nginx видит source IP docker-шлюза, а он
  попадает в этот диапазон. Отдаётся только `stub_status` (активные соединения
  и счётчики), не логи и не тела.
- `message_counter.py` в live-режиме по-прежнему не применим — проверка живой
  цепочки остаётся `scripts/live-chain-check.py` (round 206), а `message_counter`
  гоняется на дефолтном стеке-стенде.

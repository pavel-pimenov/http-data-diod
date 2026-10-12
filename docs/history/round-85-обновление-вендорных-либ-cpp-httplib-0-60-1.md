# Round 85: обновление вендорных либ (cpp-httplib v0.59.0 → v0.60.1)

## Сверка состояния

`python3 scripts/update-vendored-libs.py` (без `--ref`) — локальное дерево
полностью соответствует пинам (расхождения только в намеренно пропатченных
файлах `json/*`, `nats/CMakeLists.txt`, `civetweb.c`). Затем опрос апстрима
через `git ls-remote --tags/--heads` по всем 7 либам:

| Либа | Пин | Апстрим | Итог |
| --- | --- | --- | --- |
| nlohmann/json | v3.12.0 | v3.12.0 (последний тег) | без изменений |
| prometheus-cpp | master @ 66b6159 | HEAD master = 66b6159 | без изменений |
| civetweb | v1.16 | v1.16 (последний тег) | без изменений |
| nats.c | v3.14.0 | v3.14.0 (последний тег) | без изменений |
| **cpp-httplib** | **v0.59.0** | **v0.60.1** (последний тег) | **обновлено** |
| base64 | master @ 8d96a2a | HEAD master = 8d96a2a | без изменений |
| odpi | v26.0.0 | v26.0.0 (последний тег) | без изменений |

## Изменения

* **cpp-httplib v0.59.0 → v0.60.1** (пин `v0.60.1` @ `00afaae`): обновлены
  артефакты штатного `split.py` этого тега — `src/httplib/httplib.h` (версия
  `0.60.1`/`0x003c01`, SSE-парсер `has_id`, квалифицированный
  `friend class httplib::ws::WebSocketClient` — фикс MSVC) и
  `src/httplib/httplib.cc`. Локальных правок в них нет.
* `scripts/update-vendored-libs.py`: пин `REFS["cpp-httplib"]` → `v0.60.1`.
* `src/VENDORED-LIBS.md`: строка версии cpp-httplib и коммит-пин обновлены.

### Из релевантного апстрима (v0.60.0 + v0.60.1)

* **Suffix `Range`**: `bytes=-N` длиннее тела теперь отдаёт всё тело как 206
  (по RFC 9110 §14.1.2), а не 416; `bytes=-0` по-прежнему 416.
* **Редиректы**: после 303 тело исходного запроса больше не переотправляется
  (в т.ч. для chunked-провайдера); `Location`-путь не декодируется перед
  follow-up, относительные `Location` резолвятся по RFC 3986 §5.2.
* **WebSocket**: невалидный `Sec-WebSocket-Protocol` от сервера теперь валит
  handshake (RFC 6455 §4.1); пинги не уходят на spurious wakeup.
* **SSE**: `retry` не-цифрой игнорируется, минимальный reconnect-wait 100 мс,
  пустой `id` сбрасывает `Last-Event-ID`, пустые `data`-поля сохраняются.
* **Безопасность/парсинг**: типы SAN в Mbed TLS hostname-verify; отклонение
  амбивалентной рамки (`Transfer-Encoding` + `Content-Length`) в ответе;
  точная проверка порта в URL.
* **Windows/CryptoAPI**: интермедиаты от сервера передаются в цепочку, unknown
  revocation status принимается, CryptoAPI — единственный верификатор (не наша
  платформа, но берём целиком).
* Публичный API не изменился; переопределения из `src/cpp_httplib_config.h`
  (`SEND_BUFSIZ`/`RECV_BUFSIZ` 1 MB, `KEEPALIVE_MAX_COUNT 200`, timeouts 300s,
  `TCP_NODELAY`) в новой версии используются как раньше — правок проекта не
  потребовалось.

## Проверка

* `./rebuild-and-run.sh`: rc=0, оба образа собраны, юнит-тесты в контейнере
  `test_proxy_core` 883 assertions / 120 test cases и `test_components`
  2583 assertions / 556 test cases — `All tests passed`.
* `./health-check.sh all`: rc=0 (все endpoint'ы OK).
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0 — 1/1 POST без
  потерь/перекрёстных ответов, GET favicon валиден.
* Изменений в коде приложения нет (только вендоренный слепок + скрипт/док) —
  coverage и clang-tidy не затронуты.

# round 199: магические числа вынесены в именованные константы

## Что сделано

По коду прошёл поиск числовых литералов в продакшен-файлах (`src/*.cpp`,
`src/*.hpp` без `test_*`) — 585 вхождений. Часть из них — не «магия», а
самоописательные таблицы и значения по умолчанию, названные полями структуры
(их не трогал, см. ниже). Остальное вынесено в именованные константы.

### Новый `src/constants.hpp` — сквозные константы

| Пространство | Что внутри |
|---|---|
| `http_status` | `kOk/kBadRequest/kForbidden/kNotFound/kMethodNotAllowed/kConflict/kUnprocessableEntity/kTooManyRequests/kInternalServerError/kBadGateway/kServiceUnavailable/kGatewayTimeout` + границы классов `kSuccessMin/kSuccessMax/kRedirectMax/kClientErrorMin/kServerErrorMin` |
| `service_port` | `kProxyMetrics=19090`, `kWorkerMetrics=19091`, `kServerMetrics=19092`, `kWorkerHealth=19093` |
| `http_server_limits` | keep-alive `100`/`5s`, максимальное тело запроса `10 MB` |
| `net_limits` | границы TCP-порта `1..65535`, порты по умолчанию `80/443`, порог предупреждения о размере HTTP-пула `1000` |
| `http_timeouts` | connect `5s`, acquire HTTP-пула `30s`, graceful shutdown `30s`, шаг опроса флага shutdown `100ms` |

### HTTP-статусы вместо литералов

95 литералов статусов в 12 файлах заменены на `http_status::*`:
`request_handler.cpp`, `l2_worker.cpp`, `l2_worker_nats.cpp`,
`nats_poll_service.cpp`, `nats_push_service.cpp`, `server_handler.cpp`,
`db_query_handler.cpp`, `db_query_utils.hpp`, `response_builder.cpp`,
`json_response_utils.hpp`, `tracing_helpers.hpp`, `error_categorizer.hpp`,
`db_gateway_routing.hpp`, `trace_logger.cpp`. Проверки вида `>= 500`, `< 400`,
`== 429` тоже переведены на именованные границы — «порог ошибки сервера» и
«429» теперь не приходится угадывать по числу.

### W3C traceparent и Sentry

* `trace_logger.hpp`: длина traceparent `55`, позиции версии/trace-id/span-id/
  flags (`3/34/35/36/51/52/53/54`), длины `32`/`16` — в
  `g_traceparent_*`/`g_trace_id_hex_len`/`g_span_id_hex_len`; `validate_traceparent`
  и `parse_traceparent` больше не состоят из «голых» индексов.
* `sentry::kEventIdHexLen = 32` — один источник для event_id в `trace_logger.cpp`
  и `sentry_client.cpp` (раньше `32` стояло в двух местах).
* `g_tracing_max_cooldown_shift = 20` — предел сдвига в экспоненциальном
  cooldown-е circuit breaker-а (защита `base << shift` от переполнения).

### NATS

* `nats_client.cpp`: `kNatsReconnectWaitMs=1000`, интервал/таймаут опроса
  закрытия `5ms`/`3000ms`, буфер URL подключения `256`.
* `l2_worker_nats.cpp`: каденция цикла переподключения `200ms` на проход,
  jitter `25%`, границы backoff `1..150` проходов, drain `5000ms`, отправка
  ответа — `3` попытки с паузой `100ms`.
* `nats_poll_service.cpp`: таймаут по умолчанию `30000ms`, backoff
  переподключения `250..2000ms`, паузы повторов `1000ms` (нет responders) и
  `250ms` (пустой ответ), `1000ms` в секунде — из `TimeUtils::kMsPerSecond`.

### Тайминги, лимиты и размеры

* `main.cpp`: порты метрик/health (см. `service_port`), keep-alive
  (`100`/`5s`), `10 MB` тела, опрос shutdown `100ms`, ожидание in-flight `30s`;
  сообщения в логе теперь печатают порт из константы, а не из строки.
* `l2_worker.cpp`: acquire-таймаут HTTP-пула `30s`, тикер метрик `100ms × 50`
  (`~5s`), статусы для span-ов и circuit breaker-а.
* `stats_page.hpp`: окно по умолчанию `30` минут и границы `1..120`,
  auto-refresh `5s`, геометрия спарклайна `188×26`, минимум точек `2`,
  число серий-кандидатов `16`, секунд в минуте.
* `metrics_history.hpp`: `15s`/`8` серий на семейство/`240` сэмплов — теперь
  единый источник для значений по умолчанию и для `app_context.cpp`.
* `dynamic_labeled_family.hpp`: TTL `300s` и предел `10000` значений метки;
  `proxy_init.cpp` передаёт именно эти константы вместо `300, 10000`.
* `crash_handler.hpp`: ожидание crash-репорта `80 × 50ms` (~4s), порты Sentry
  `80/443`, локальный backtrace использует уже существующий `kMaxFrames`
  вместо второго литерала `128`.
* `sentry_client.cpp`: event_id `32`, размер батча отправки `8`, порты
  `80/443`, предел TCP-порта, перевод `ms → s` через `TimeUtils`.
* `json_schema_validator.hpp`: ограничения тела запроса `10 MB`, пути `2048`,
  тела ответа `50 MB`.
* `logger.hpp`: `1000ms` в секунде, ротация файла `10 MB × 3`.
* Прочее: длина превью тела в логе `100` (`common_utils.cpp`), глубина
  backtrace `/debug/stacktrace` `32` (`request_handler.cpp`), база года
  `1900` и максимум случайного суффикса `999999` (`request_id_generator.cpp`),
  `1000ms` в rate limiter-е, порты по умолчанию в `url_utils.hpp`/
  `l2_routing.hpp`, предел TCP-порта в `http_client.cpp`, длина префикса
  `"HTTP "` в `trace_logger.cpp`, `TimeUtils::kUsPerMs/kMsPerSecond/kUsPerSecond`
  для переводов единиц.

### Что осознанно оставлено как есть

* `config.hpp` — значения по умолчанию у полей конфигурации: они уже названы
  именем поля (`m_timeout_ms`, `m_batch_size`, …) и меняются через окружение;
  вынесение их в отдельные константы только разорвало бы связь
  «поле = значение по умолчанию».
* `metrics_manager.hpp` — таблицы границ гистограмм: это данные, а не магия,
  имена массивов (`m_latency_ms_to_10s`, `m_size_100b_to_5mb`) описывают их
  лучше любой константы.
* `crash_handler.hpp` — размеры стековых буферов (`16/32/64/256/512`), у них
  разные назначения и они видны рядом с `snprintf`/`read`.
* `pg_value_utils.hpp` — OID-ы PostgreSQL уже были именованы.
* `retry_handler.hpp`/`retry_utils.hpp` — значения по умолчанию параметров
  (это API, а не константы поведения).
* Вендоренные библиотеки (`httplib`, `nlohmann`, `prometheus-cpp`, `base64`,
  `nats`) по правилам проекта не трогаются.

Поведение не изменилось: значения констант совпадают с прежними литералами,
тесты правились только там, где они проверяют контракт (ни одного изменения
ожидаемых чисел).

## Проверка

* `./rebuild-and-run.sh`: rc=0, сборка образов и юнит-тесты в стадии сборки —
  `All tests passed` (2986 assertions в 615 test cases + 946 assertions в 125
  test cases), сервисы подняты.
* `./health-check.sh all`: rc=0 (health/ready прокси и воркера, `/metrics`
  на 19090/19091/19092, nginx, jaeger).
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0 (1/1 POST,
  GET favicon валиден).
* `./scripts/run-cppcheck.sh`: «no errors/warnings/performance/portability
  issues in project files».
* `./scripts/run-clang-tidy.sh`: rc=0; из предупреждений по изменённым файлам
  два были внесены этим раундом (implicit widening в `constants.hpp` и
  `logger.hpp`) — исправлены через `std::size_t{…}`, остальные предупреждения
  существовали до раунда.
* Форматирование: строки в диффе — не длиннее 80 колонок (clang-format на
  хосте не установлен, выравнивание сделано вручную по стилю `src/.clang-format`).

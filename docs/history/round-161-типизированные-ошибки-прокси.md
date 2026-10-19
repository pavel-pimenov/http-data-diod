# round 161 — типизированные ошибки клиента у прокси: `l2_proxy_client_request_errors_total{type}` (P1 #3 из TODO, вторая половина)

## Что сделано

Вторая половина пункта 3 TODO (первая — воркерная — была в round 159): у прокси был один
неразмеченный счётчик `l2_proxy_client_request_errors_total`, по которому в алерте нельзя было
отличить «клиент прислал мусор» от «мы не дождались воркера» от «сработал rate-limit» — в SLO-панелях
все отказы складывались в одну кривую.

Метрика получила метку `type` (`HttpErrorType`), **имя не менялось** — это важно: `README`,
`CATALOG` golden-check, PromQL-панели и Perses остались теми же, обновился только разрез
(в четырёх источниках по-прежнему 91 метрика). Семейство стало
`prometheus::Family<prometheus::Counter>` в `ProxyMetrics::m_client_errors`.

## Где выводится тип (централизованно)

Тип определяется ровно в одном месте — новом `handle_client_error()` (`src/common_utils.cpp`),
который вызывается из прокси-хелпера `fail_request_categorized()` (`src/common_utils.hpp`).
Классификатор — уже существующий `categorize_http_error()` из `src/error_categorizer.hpp` (общий
с воркером), при этом используется и статус, и текст:

| Отказ в прокси | Что получается | Почему |
|---|---|---|
| `400 Invalid JSON in request body` | `CLIENT_ERROR` | 4xx — статус классифицирует точно |
| `429` (rate limit) | `RATE_LIMIT_ERROR` | то же |
| `504 Timeout waiting for response` | `TIMEOUT_ERROR` | 5xx одинаков у разных причин → решают ключевые слова текста |
| `500 Invalid response format` | `PROTOCOL_ERROR` | ключевое слово `invalid` → протокольная/форматная ошибка |
| `500 Failed to queue request` (NATS) | `SERVER_ERROR` | ключевых слов нет → fallback «наша сторона» |

Логика: у 4xx статус однозначен и не зависит от формулировки сообщения, поэтому он и выигрывает;
у 5xx статус у всех причин одинаков (`500`/`504`), и чтобы не потерять «воркер не ответил вовремя»
в общем `SERVER_ERROR`, вторым шагом идёт keyword-проход по тексту, а `SERVER_ERROR` остаётся
значением по умолчанию. Это ровно то, ради чего пункт и заводился: таймаут ожидания воркера
становится видимым отдельной серией (в дашборде — отдельная линия/легенда).

Предсоздание: все девять значений `HttpErrorType` получают нулевую серию в
`init_proxy_metrics()` (тем же приёмом, что переходы circuit breaker в round 155 и
`l2_worker_l2_call_errors_total` в round 159) — на здоровом прокси отказов нет, а prometheus-cpp
отдаёт лейбл-семейство только после первого `Add()`. Побочный плюс: семейство теперь есть в
`/metrics` всегда, а не только после первой ошибки — раньше golden-check мог его не увидеть на
чистом стенде (для ленивых семейств там есть `--all`).

## Что не тронули

- **`fail_request()` / `handle_error()`** остались с `prometheus::Counter *` — ими пользуются
  l2-server (`m_request_errors`) и тесты. Для прокси добавлен отдельный inline-хелпер
  `fail_request_categorized()`, который логирует через `handle_client_error()` и пишет ответ через
  тот же `set_json_error_response()`. Дублирования логики нет, разошлись только две строки вызова
  метрики.
- **`l2_server_request_errors_total`** не размечен: l2-server по правилам проекта — тестовый стаб,
  его наблюдаемость не развиваем.
- **Тексты ошибок/логи** не менялись: `handle_client_error()` пишет тот же `Logger::error`, что и
  прежний `handle_error(..., log_error=true)`, так что логи остались байт-в-байт.

## Точки инкремента

Четыре места (все — там же, где раньше считался общий счётчик):

- `src/request_handler.cpp` — отказ `400` на невалидном JSON тела;
- `src/request_handler.cpp::fail_backend_request()` — ошибка бэкенда (статус передаётся как есть:
  `500`/`504`);
- `src/request_handler.cpp::reject_rate_limited()` — `429` (rate limiter);
- `src/response_builder.cpp` — `500 Invalid response format` при разборе ответа L2.

`stats_logger` у семейства не мог вызвать `Value()` (его нет у `Family`) — в ветке прокси теперь
суммируются серии (`Collect()` → по `metric_family.metric`), то есть `/stats` показывает ровно ту
же цифру, что и раньше.

## Дашборд

Панель L2 Прокси «Запросы в секунду» (id 32): цель `B` была
`sum(rate(l2_proxy_client_request_errors_total[5m]))` с легендой «Ошибок/с», стала
`sum by (type) (rate(...[5m]))` с легендой `{{type}}` — сумма линий равна прежней «Ошибок/с», но
теперь на графике видно, за чей счёт отказ. Ссылки SLO-панелей (`sum(...)` без группировки) при
появлении метки продолжают работать без правок — суммирование сворачивает метку `type`.

## Тесты

- `AppContext: proxy pre-creates the client error type series` — проверяет, что в реестре прокси
  лежат все девять `type` (сравнение отсортированного списка с ожидаемым), то есть ловит и потерю
  предсоздания, и расхождение с enum.
- `Common utils: fail_request_categorized labels the error type` — прогоняет пять отказов через
  хелпер и проверяет распределение по сериям: `400` → `CLIENT_ERROR`, `429` → `RATE_LIMIT_ERROR`,
  `504 Timeout...` → `TIMEOUT_ERROR`, `500 Invalid...` → `PROTOCOL_ERROR`, `500 Failed to queue` →
  `SERVER_ERROR`. Тест покрывает обе ветки классификации и fallback.
- Существующие тесты `fail_request`/`handle_error` не менялись (их сигнатуры те же).

## Проверка

- `metrics-consistency-check.py --offline` — 91 метрика; runtime-гейт — `registered=91 dashboards=91
  perses=91 readme=91 golden=91`.
- `generate-grafana-dashboards.py --check`, `generate-perses-dashboards.py --check` (8/8),
  `lint-python.py` — без замечаний.
- Сборка/тесты, health, smoke, `ci-gate.sh runtime` (golden с предсозданными сериями), cppcheck,
  clang-tidy `--all`, coverage — см. прогон.

# TODO: кандидаты в метрики (обсервабилити)

> Планировочный документ. Здесь **нет** регистраций метрик — это очередь идей, разобранная
> по подсистемам с типом, метками, местом регистрации, ценностью и рисками кардинальности.
> Реализация каждой позиции — отдельный раунд (или пачка родственных метрик в одном раунде).

## Как реализовывать (правило четырёх источников)

Имя метрики обязано совпадать в четырёх местах, иначе падает гейт
`scripts/metrics-consistency-check.py` (pre-commit + CI):

1. регистрация в C++ — `src/app_context.cpp` (или `src/circuit_breaker.hpp`/`pg_*`/трейсинг-хелперы,
   где метрика живёт рядом с владельцем), реестр: свой режимный реестр либо общий `l2_common`;
2. PromQL/панель — `scripts/generate-grafana-dashboards.py`;
3. строка каталога — README, раздел «Метрики Prometheus (полный каталог)» (+ синхронизировать
   подразделы про наблюдаемость и rate limiter, если метрика оттуда);
4. список семейств — `scripts/metrics-golden-check.py` (семейства, появляющиеся только под
   трафиком, — в секцию дополнительной проверки `--all`).

Чек-лист перед реализацией:

- [ ] имя уникально и не совпадает с существующим (см. README-каталог);
- [ ] метки **только с ограниченной кардинальностью** (никаких `request_id`, сырых путей, тел);
- [ ] выбран правильный реестр: общие tracing/sentry — `l2_common`, остальное — режимный;
- [ ] для `history`-семплера (`metrics_history.hpp`) — нужен ли сэмпл в истории;
- [ ] при новой env-переменной (например, лимит кардинальности) — добавить её в `docker-compose.yml`;
- [ ] прогнать `scripts/metrics-consistency-check.py --offline`, затем полный цикл сборки.

**Не делать:** per-mode метрики/панели/отдельный `/stats` для `l2-server` (это не продакшен-стаб
для worker), тесты/покрытие по Oracle-исполнителю (`src/db_query_executor_oracle.cpp`).

## Что уже покрыто (чтобы не дублировать)

| Подсистема | Есть сейчас |
|---|---|
| proxy HTTP | `l2_proxy_client_requests_total`, `responses_total{status}`, `request_duration_seconds`, `in_flight_requests`, `request_size_bytes`/`response_size_bytes`, `bytes_received_total`/`bytes_sent_total`, `client_request_errors_total{type}`, `rejections_total{reason}`, `health_ready` |
| proxy rate limiting | `l2_rate_limiter_tokens`, `l2_rate_limiter_rejected_total`, `l2_per_ip_*` (requests/rejected/ips_tracked), `l2_proxy_per_client_id_*` (requests/rejected/latency), `l2_proxy_per_ip_*` |
| proxy duplicate | `duplicate_requests_total`, `duplicate_posts_detected_total`, `duplicate_tracked_clients`, `per_client_id_duplicate_requests_total`, `per_client_id_duplicate_rejected_total` |
| proxy DB/NATS-клиент | `db_requests_total`, `db_request_duration_seconds`, `db_nats_request_duration_seconds`, `nats_connected`, `nats_connection_creates_total`, `nats_connection_errors_total`, `nats_errors_total`, `nats_requests_total`, `nats_request_duration_seconds`, `nats_poll_attempts_total`, `nats_poll_attempt_duration_seconds`, `nats_poll_retry_wait_seconds` |
| proxy task queue | `l2_proxy_task_queue_enqueued_total`, `_rejected_total`, `_wait_seconds`, `_size`, `_capacity` |
| HTTP-пул (общий) | `l2_http_pool_active_clients`, `_available_clients`, `_client_acquisitions_total`, `_client_releases_total`, `_stale_evictions_total` |
| worker | `requests_processed_total`, `responses_total`, `request_duration_seconds`, `in_flight_requests`, `queue_size`, `queue_capacity`, `bytes_*`, `nats_connected`, `health_ready`, `graceful_shutdown_seconds`, `processing_json_errors_total`, `processing_validation_errors_total` |
| worker DB | `db_requests_total`, `db_query_duration_seconds`, `db_pool_connections`, `db_pool_capacity`, `db_pool_exhausted_total`, `db_pool_acquire_duration_seconds`, `db_gateway_ready` |
| worker L2 | `l2_calls_total`, `l2_call_duration_seconds`, `l2_errors_total`, `l2_call_errors_total{type}`, `l2_response_size_bytes`, `circuit_breaker_state`, `circuit_breaker_transitions_total{to}`, `circuit_breaker_rejected_total` |
| tracing/sentry (`l2_common`) | `l2_tracing_spans_sent_total`, `_failed_total`, `_spans_dropped_total{reason}`, `_queue_size`, `_queue_time_seconds`, `_send_latency_seconds`, `_last_send_duration_seconds`, `sentry_transactions_sent_total`/`_failed_total`, worker `sentry_events_sent_total`/`_failed_total`/`sentry_queue_size`, `l2_common_build_info{version,mode}`, `l2_common_stats_history_samples_total{outcome}`, `l2_common_stats_history_buffer_size{history}`, `l2_common_crash_dumps` |

> **Очередь P1–P2 закрыта в round 202.** Все пункты ниже либо реализованы, либо отклонены с
> причиной — открытых кандидатов нет (P3 остаётся списком «не спешить»). Аудит правил и
> политика «какие метрики осознанно без алертов и почему» — в README, раздел «Какие метрики
> намеренно без алертов».

---

## P1 — высокая ценность, низкий риск (внедрять первыми)

### 1. Circuit breaker: переходы и отклонения ✅ реализовано (round 155)
Сейчас только gauge состояния; внутренние `Counters` (failure/success в `src/circuit_breaker.hpp`)
никуда не экспортируются — по дашборду не видно, как часто брейкер «щёлкает».

| Метрика | Тип | Метки | Что закрывает |
|---|---|---|---|
| `l2_worker_circuit_breaker_transitions_total` | counter | `to` (open/half_open/closed) | частота открытия/восстановления, алерт «брейкер часто открывается» |
| `l2_worker_circuit_breaker_rejected_total` | counter | — | сколько запросов отброшено в OPEN (fast-fail), доля от `l2_calls_total` |

Регистрация: `src/app_context.cpp` рядом с `l2_worker_circuit_breaker_state`; у `CircuitBreaker`
добавить `set_counters(...)` по образцу `set_gauge(...)`. Кардинальность: `to` — 3 значения.
Реализовано в round 155: три серии `to` предсоздаются в `init_worker_metrics()`, чтобы
семейство экспортировалось (как 0) ещё до первого перехода — иначе prometheus-cpp не отдал бы
лейбл-семейство в `/metrics` в здоровом стеке и golden check его бы не нашёл.

### 2. DB-пул PostgreSQL: исчерпание и время получения соединения ✅ реализовано (round 157)
Сейчас видны только idle/active из `db_pool_connections`; под нагрузкой отсутствие
«сколько раз не дождались» и «сколько стоит получить соединение» — слепая зона для алертов
на сатурацию.

| Метрика | Тип | Метки | Что закрывает |
|---|---|---|---|
| `l2_worker_db_pool_capacity` | gauge | `db` | настроенный лимит пула (`pool_max`) — знаменатель для сатурации |
| `l2_worker_db_pool_exhausted_total` | counter | `db` | сколько acquire отброшено, потому что все соединения заняты (пул выжат) |
| `l2_worker_db_pool_acquire_duration_seconds` | histogram | `db` | время получения соединения (из пула или новое подключение) |
| ~~`l2_worker_db_pool_waiting_requests`~~ | — | — | **отклонено при реализации**: пул неблокирующий (try-acquire), очереди ожидания нет — gauge был бы вечно 0 |

Реализовано в round 157: метки `db` (как у `db_pool_connections`/`db_requests_total`) вместо
запланированных «без меток» — иначе несколько БД сливались бы; серии предсоздаются в
`DbExecutorBase::set_pool_saturation_metrics()` (prometheus-cpp отдаёт лейбл-семейство только
после первого `Add()`). Роль `waiting_requests` играет пара `active`/`capacity` из
`db_pool_connections` + всплеск `exhausted_total`. Пул-сатурация заполняется из
`PostgresQueryExecutor::Impl`; Oracle-исполнитель наследует no-op (его обсервабилити вне области).

### 3. Типизированные ошибки (использовать уже готовый классификатор) ✅ реализовано (round 159: воркер; round 161: прокси)
`src/error_categorizer.hpp` уже различает `HttpErrorType`/`L2ErrorType`/`ProcessingErrorType`,
но метрики `l2_proxy_client_request_errors_total` и `l2_worker_l2_errors_total` без метки причины —
в алерте не отличить таймаут от TLS/протокольной ошибки.

| Метрика | Тип | Метки | Что закрывает |
|---|---|---|---|
| `l2_worker_l2_call_errors_total` ✅ | counter | `type` (`L2ErrorType`) | разбор ошибок вызова L2 по причине |
| `l2_proxy_client_request_errors_total{type}` ✅ | counter | `type` (`HttpErrorType`) | разбор ошибок клиента к worker |

Сделано в round 159: `l2_worker_l2_call_errors_total` рядом с неразмеченным
`l2_worker_l2_errors_total` (инкремент в `src/l2_worker.cpp` там же, где сейчас считается общий
счётчик; все пять серий `type` предсоздаются в `init_worker_metrics()` — в здоровом стеке ошибок
нет, а prometheus-cpp отдаёт лейбл-семейство только после первого `Add()`).

Сделано в round 161 (прокси): `l2_proxy_client_request_errors_total` **расширен меткой `type`**
(`Counter` → `Family<Counter>`, имя не менялось, поэтому golden/README/дашборды остались в силе).
Тип выводится централизованно в новом `handle_client_error()` (`common_utils.cpp`) из пары
«статус + текст ошибки»: 4xx классифицирует статус (`400` → `CLIENT_ERROR`, `429` →
`RATE_LIMIT_ERROR`), у 5xx (статус одинаков у разных причин) выигрывают ключевые слова текста
(`504 Timeout waiting for response` → `TIMEOUT_ERROR`, `500 Invalid response format` →
`PROTOCOL_ERROR`), а `SERVER_ERROR` остаётся fallback'ом. Все девять значений enum
предсоздаются в `init_proxy_metrics()`. Четыре точки инкремента перешли на новый inline-хелпер
`fail_request_categorized()` (`request_handler.cpp` ×3, `response_builder.cpp` ×1) — старые
`fail_request`/`handle_error` не тронуты, поэтому l2-server и тесты остались на прежних
неразмеченных счётчиках (по правилу проекта l2-server не развиваем). `stats_logger` суммирует
серии семейства; на дашборде L2 Прокси панель «Запросы в секунду» показывает разрез
`sum by (type)`.

### 4. NATS-клиент: таймауты, реконнекты, подписки ✅ реализовано (round 174)
`connection_creates_total` смешивает первый коннект и реконнекты; явного счётчика таймаутов нет,
хотя `nats_poll_attempts_total` есть.

| Метрика | Тип | Метки | Что закрывает |
|---|---|---|---|
| `l2_proxy_nats_timeouts_total` | counter | — | таймауты NATS-запроса (отдельно от общих `nats_errors_total`) |
| `l2_proxy_nats_reconnects_total` | counter | — | нестабильность NATS-сервера |
| `l2_proxy_nats_subscriptions` | gauge | — | число активных подписок (утечка подписок) |

Регистрация: `app_context.cpp` + `src/nats_client.hpp` (точки reconnect/callback). Кардинальность: нет меток.

Реализовано в round 174: `l2_proxy_nats_timeouts_total`,
`l2_proxy_nats_reconnects_total` и `l2_proxy_nats_subscriptions` (gauge числа
активных подписок). У `NatsClient` появилась необязательная привязка метрик
(`NatsClient::Metrics` + `set_metrics()`, по образу circuit breaker) — клиент
используется и прокси, и воркером, а счётчики живут в разных реестрах.

### 5. Ретраи и failover к L2 ✅ реализовано (round 173: повторы; failover не применим)
`l2_calls_total` есть, но нет «сколько попыток из-за ретраев» и «сколько раз переключились
на резервный L2» — ключевое для понимания деградации бэкенда.

| Метрика | Тип | Метки | Что закрывает |
|---|---|---|---|
| `l2_worker_retries_total` ✅ | counter | `outcome` (retried/exhausted) | цена ретраев, исчерпание попыток |
| ~~`l2_worker_l2_failover_total`~~ | — | — | **отклонено при реализации**: в NATS-режиме воркер публишует в NATS по списку `L2_SERVER_URLS`, а не переключает «резервный» HTTP-эндпоинт — события «переключились на резервный адрес» в коде нет (выбор следующего адреса при недоступности — это и есть `l2_worker_l2_calls_total` по разным URL) |

Кардинальность: `from×to` ограничены списком endpoint'ов из конфига (обычно ≤ единицы);
при росте списка — рассмотреть метку `to` только. Регистрация: `app_context.cpp`, инкремент в `src/l2_worker.cpp`.

Реализовано в round 173: `l2_worker_retries_total{outcome}` (`retried`/`exhausted`) —
цена повторов вызова L2; список исходов в `app_context.hpp` (`g_worker_retry_outcomes`),
все серии предсоздаются при старте, инкременты — в цикле `execute_l2_call_with_retry`.

### 6. Единая причина отказов на proxy ✅ реализовано (round 170)
`rejections_total{reason}` — зонтичная метрика для дашборда «почему отказываем»,
чтобы не складывать вручную четыре разных счётчика.

| Метрика | Тип | Метки | Что закрывает |
|---|---|---|---|
| `l2_proxy_rejections_total` | counter | `reason` (rate_limit_per_ip / rate_limit_per_client_id / rate_limit_tokens / duplicate / in_flight_limit / body_too_large) | единый разрез отказов, алерт на скачок любой причины |

Кардинальность: закрытый enum причин. Регистрация: `app_context.cpp`, инкремент в
`rate_limiter*.hpp`, `duplicate_detector.hpp`, `request_handler.cpp`.

Реализовано в round 170: `l2_proxy_rejections_total{reason}` с закрытым набором
причин (`rate_limit_global`/`rate_limit_per_ip`/`duplicate`/`invalid_json`) —
enum `RejectionReason` в `src/error_categorizer.hpp`, все серии предсоздаются при
старте, инкремент в одном методе `RequestHandler::count_rejection()` (метки из
enum, а не литералы в местах вызова). Частные счётчики остались: они считают
*события*, umbrella — *отказы*.

### 7. Tracing/sentry: потери и невалидный traceparent ✅ реализовано (round 167: потери спанов; round 172: потери Sentry и битые traceparent)
Есть `spans_failed_total`, но потеря из-за переполнения очереди/выключенного экспортёра неотличима.

| Метрика | Тип | Метки | Что закрывает |
|---|---|---|---|
| `l2_tracing_spans_dropped_total` ✅ | counter | `reason` (`queue_full`/`breaker_open`/`export_error`/`shutdown_flush`) | потеря трейсов и её причина |
| `l2_tracing_traceparent_invalid_total` | counter | — | клиенты присылают битый traceparent (шум/атака) |
| `l2_common_sentry_dropped_total` | counter | `reason` (queue_full/disabled) | потеря событий Sentry |

Регистрация: общий реестр `l2_common` (`src/trace_logger.cpp`/`sentry_client.hpp` + `app_context.cpp`).
Кардинальность: закрытый enum. При добавлении — обновить подраздел README про трассировку.

Сделано в round 167: `l2_tracing_spans_dropped_total{reason}` с четырьмя причинами
(очередь переполнена / брейкер сбросил партию / отправка не удалась после всех
попыток / не успели на финальном сбросе) — список причин в одном месте
(`g_tracing_drop_reasons`), все серии предсоздаются при старте; попутно исправлен
перепутанный счётчик (путь Sentry-брекера увеличивал Jaeger-овский `spans_failed`).

Сделано в round 172: `l2_tracing_traceparent_invalid_total` (клиентские
`traceparent`, которые присутствуют, но не разбираются — запросы без заголовка не
считаются) и `l2_worker_sentry_events_dropped_total{reason}` (Sentry-события,
потерянные до попытки доставки; имя выбрано по соседям
`l2_worker_sentry_events_*`, а не как в плане `l2_common_sentry_dropped_total` —
чтобы не плодить второй префикс для того же реестра).

### 8. Build info и uptime ✅ реализовано (round 160)
Стандартная, но отсутствует — сильно упрощает корреляцию деплоя с метриками.

| Метрика | Тип | Метки | Что закрывает |
|---|---|---|---|
| `l2_common_build_info` ✅ | gauge (=1) | `version`, `mode` (proxy/worker/server) | версия сервиса на дашборде без захода в логи |

Регистрация: `l2_common`, значение из `g_l2_proxy_version` (`src/main.cpp`).
Реализовано в round 160: семейство `l2_common_build_info` регистрируется в `init_common()`
(`src/app_context.cpp`) и сразу получает одну серию со значением `1`; версия берётся из
`g_l2_proxy_version`, режим — из `m_config.m_app.m_mode`. Метка `mode` добавлена к запланированным
`version` — без неё в одном VictoriaMetrics серии трёх сервисов схлопнулись бы в одну (в проекте
уже есть срез по инстансу `vm`, но режим отличает proxy/worker/server даже при одинаковой версии).
Отдельный `l2_common_uptime_seconds` не делался: uptime даёт `process_start_time_seconds` из
стандартного экспортёра процессов, дублировать его своим gauge'ом смысла нет.

### 9. Ёмкость очередей (рядом с существующими gauge'ами) ✅ реализовано (round 169: воркер, round 191: прокси)
`queue_size` есть, но без знаменателя нет алерта на сатурацию.

| Метрика | Тип | Метки | Что закрывает |
|---|---|---|---|
| `l2_worker_queue_capacity` | gauge | — | лимит очереди воркера |
| `l2_proxy_task_queue_capacity` | gauge | — | лимит очереди задач proxy |
| ~~`l2_worker_task_queue_wait_seconds`~~ | — | — | **не применимо**: `TimedTaskQueue` оборачивает только HTTP-сервер прокси; воркер обрабатывает NATS-сообщения и своей очереди задач не имеет (глубина пула — `l2_worker_queue_size`/`l2_worker_queue_capacity`) |

Регистрация: `app_context.cpp` (ёмкость — из конфига, `timed_task_queue`/`thread_pool_wrapper`).

Сделано в round 169: `l2_worker_queue_capacity` — ёмкость очереди пула потоков
воркера (предел из `L2_WORKER_QUEUE_SIZE` либо автоматический: 8 задач на поток
× `L2_WORKER_THREADS`; в `ThreadPool` добавлены аксессоры
`effective_queue_capacity()`/`max_queue_capacity()`).

Сделано в round 191: у прокси появились обе величины — `l2_proxy_task_queue_size`
(глубина: принятые соединения, которые ещё не забрал воркер-тред) и
`l2_proxy_task_queue_capacity` (предел из нового `L2_PROXY_MAX_QUEUED_REQUESTS`,
0 = без предела). Считать глубину пришлось в обёртке `TimedTaskQueue`: список
задач у httplib приватный, а опроса из фонового тикера нет — поэтому глубина
считается в точке приёма задачи и в точке её старта. Ненулевой предел
превращает переполнение очереди в отказ соединения вместо неограниченного
роста памяти.

Остаток закрыт в round 202: `l2_worker_task_queue_wait_seconds` **отклонена** — у воркера
нет HTTP-очереди задач (`TimedTaskQueue` оборачивает только httplib-сервер прокси), а
ожидание в пуле потоков уже видно по `l2_worker_queue_size`/`l2_worker_queue_capacity`.

---

## P2 — полезно, но требует аккуратности

> Все пункты закрыты в round 202: реализованы либо отклонены с причиной в таблице.

> Пункт про ожидание в HTTP-пуле закрыт в раунде 177: пул эти величины уже
> считал, но воркер их не отдавал, а сами метрики пула жили в реестре прокси
> вместо общего.

| Метрика | Тип | Метки | Зачем / риск |
|---|---|---|---|
| ~~`l2_worker_l2_server_call_duration_seconds`~~ ✅ | histogram | `backend` | латентность по каждому бэкенду; меток столько же, сколько endpoint'ов в конфиге (сделано в раунде 193 ✅; метка названа `backend`, а не `l2_server`: гейт сверки метрик ищет в исходниках литералы `l2_*` и принял бы имя метки за ещё одну метрику) |
| ~~`l2_http_pool_wait_duration_seconds`~~ ✅ | histogram | — | ожидание свободного pooled-клиента (сделано в раунде 177 как `l2_http_pool_client_acquisition_duration_seconds` + `l2_http_pool_client_acquisition_timeouts_total`; заодно метрики пула перенесены из реестра прокси в общий) |
| ~~`l2_worker_request_validation_errors_total`~~ ✅ | counter | `field` | разбор ошибок валидации по полю; риск — поле свободного текста, брать только enum-поле (сделано в раунде 195 ✅ — метка берётся из закрытого набора валидатора: `method`/`path`/`body` + обязательные поля, текст причины в метку не попадает: он содержит клиентские значения) |
| ~~`l2_worker_db_query_errors_total`~~ ✅ | counter | `error_class` (SQLSTATE-класс) | разбор ошибок SQL; метка — только класс (2 символа), не код сообщения (сделано в раунде 180 ✅) |
| ~~`l2_worker_retry_attempts`~~ | — | — | **отклонено в round 202**: число попыток выводится из `l2_calls_total` и `l2_worker_retries_total{outcome="retried"}`; отдельная гистограмма дублировала бы их и шумела (почти все вызовы — 1 попытка) |
| ~~`l2_common_metrics_render_duration_seconds`~~ ✅ | histogram | — | стоимость рендера `/metrics` (важно при росте кардинальности) (сделано в раунде 184 ✅) |
| ~~`l2_common_stats_page_render_duration_seconds`~~ ✅ | histogram | — | то же для `/stats` (сделано в раунде 194 ✅) |
| ~~`l2_common_stats_history_samples_total`~~ ✅ | counter | `outcome` (ok/error/no_registry) | здоровье истории метрик (`metrics_history.hpp`) (сделано в раунде 190 ✅) |
| `l2_common_stats_history_buffer_size` | gauge | `history` (proxy/worker/server/common) | заполнение буфера истории ✅ (round 200; метка `history` добавлена вынужденно: один процесс держит четыре истории, без метки значения четырёх сэмплеров перетирали бы друг друга) |
| ~~`l2_proxy_not_ready_total`~~ ✅ | counter | `reason` | почему proxy не в ready (диагностика healthcheck) (сделано в раунде 186 ✅) |
| ~~`l2_worker_not_ready_total`~~ ✅ | counter | `reason` | то же для worker (сделано в раунде 186 ✅) |
| ~~`l2_common_crash_dumps_total`~~ ✅ | gauge | — | записанные стектрейсы (`src/crash_handler.hpp`) (сделано в раунде 200 как **gauge** `l2_common_crash_dumps`, а не counter: процесс, записавший дамп, тут же завершается через `raise()`, поэтому инкремент в обработчике сигнала никто не успел бы соскрейпить; значение — снимок каталога дампов, читается с диска перед каждой экспозицией; каталог общий для всех сервисов, поэтому в панелях/алертах нужен `max()`) |
| ~~`l2_proxy_duplicate_cache_evictions_total`~~ ✅ | counter | `reason` (ttl/cap) | вытеснение из дедуп-кэша (косвенно — повторные посты) (сделано в раунде 192 ✅ — четыре причины: `body_ttl`/`body_cap`/`client_ttl`/`client_cap`) |
| `l2_proxy_per_client_id_dropped_labels_total` | counter | — | срабатывание cap в `dynamic_labeled_family.hpp` (иначе потеря кардинальности молчалива) ✅ round 187; алерт не заводим — TTL-эвикция это нормальный churn, а `reason` в метке нет (см. README «Какие метрики намеренно без алертов») |
| ~~`l2_worker_thread_pool_active_workers`~~ | — | — | ✅ круг 188 как `l2_worker_active_workers` (занятые воркеры пула); в round 202 сверено, что дубля не осталось |
| ~~`l2_worker_timed_task_queue_pending`~~ | — | — | **не применимо**: `TimedTaskQueue` есть только у HTTP-сервера прокси, у воркера такой очереди нет |

## P3 — под вопросом / не спешить

- `l2_proxy_startup_seconds` — время до полной готовности: полезно для SLO старта, но сэмпл
  одноразовый; скорее в лог, чем в метрику.
- `l2_proxy_config_validation_errors_total{field}` — ошибки конфига фатальны на старте
  (процесс не поднимается), метрику некому будет прочитать; остаётся лог.
- per-connection метрики http-клиента (DNS/TLS-handshake отдельно) — ценность умеренная,
  библиотека даёт скудные точки; при этом нужен разрез по хосту (кардинальность).
- `l2_tracing_export_batch_size` — исторически полезно, но при батчевом экспортёре шумно.
- отдельные счётчики «drop-oldest» в `timed_task_queue`, если вытеснение вообще не происходит на
  практике — сначала проверить по коду, есть ли такая ветка (иначе метрика всегда 0 и это шум).

## Отклонено осознанно

- **Любые per-mode метрики/панели/свой `/stats` для `l2-server`** — по правилам проекта это
  тестовый стаб для worker; в его реестр `l2_common` подмешивается как есть, нового не добавляем.
- **Метрики Oracle-исполнителя** — покрытие и обсервабилити DB-слоя развиваем только по PostgreSQL.
- **Метки `request_id`, сырой путь/URL, тело запроса, `client_id` в новых семействах** —
  неограниченная кардинальность; per-client-id уже существует как отдельные семейства с TTL/cap.

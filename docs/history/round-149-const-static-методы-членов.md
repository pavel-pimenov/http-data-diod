# round 149 — clang-tidy: `const`/`static` у методов-членов (36 находок)

## Что сделано

Следующая пара проверок из того же расширенного набора (round 148 отобрал
«дешёвые» пять, здесь — две про квалификаторы членов):

```
readability-make-member-function-const,
readability-convert-member-functions-to-static,
```

Полный свип (`--all`, 47 TU) дал **36 уникальных находок**: 24 «метод может быть
`const`» и 12 «метод может быть `static`».

Правки применены не руками, а фиксами самого clang-tidy
(`clang-tidy ... --checks='-*,readability-make-member-function-const,readability-convert-member-functions-to-static' -fix-errors`):
фикс-иты проверок точнее ручной правки (они трогают и объявление в заголовке, и
определение в `.cpp`, и порядок `static`/`const`). Важная деталь: без
`-fix-errors` clang-tidy **отказывается** писать фиксы, если в TU есть ошибки
компиляции («Found compiler errors, but -fix-errors was not specified. Fixes
have NOT been applied.»), а в каждом TU проекта есть известный мусор
fmt/spdlog (`call to consteval function ... is not a constant expression`),
который гейт отфильтровывает по путям (`/usr/`). Поэтому `-fix-errors`.
Правки затронули только проектные файлы (проверено `git status`: ни одного
файла из `nats/src`, `httplib`, `base64` и прочих вендорных каталогов).

### `const` (24)

- `app_context.hpp/.cpp` — `stop_stats_history()` (только читает `unique_ptr`
  членов и дёргает `->stop()`: `get()` у `unique_ptr` сам константный).
- `l2_worker.hpp/.cpp` — `update_queue_size_metric`, `record_bytes_sent`,
  `record_l2_call_metrics`, `parse_request_data`, `extract_request_metadata`,
  `create_tracing_spans`.
- `l2_worker_nats.cpp` — `send_nats_response_impl`, `observe_db_query_outcome`.
- `nats_poll_service.hpp/.cpp` — `poll_notify_resend`, `poll_sleep_backoff`.
- `nats_push_service.hpp/.cpp` — `push_request`.
- `request_handler.hpp/.cpp` — `handle_crash_test`, `handle_duplicates`,
  `fail_backend_request`, `setup_tracing`, `send_db_gateway_error`.
- `server_handler.hpp/.cpp` — `send_response_with_trace`.
- `trace_logger.hpp/.cpp` — `record_sink_failure` (общий для Jaeger- и
  Sentry-брекеров).
- `test_nats_live.cpp` — `wait_ready` тестового `ServerFixture`.
- `db_query_executor_oracle.cpp` — `update_pool_gauges` у `OracleQueryExecutor::Impl`
  (Oracle-исполнитель тестами не покрываем, но `const` — чисто сигнатурная
  правка, покрытие/ветви по нему не гоняются).

### `static` (12)

- `l2_worker.hpp/.cpp` — `extract_l2_server_span_id`,
  `prepare_response_headers`, `prepare_response_data` (не читают состояние
  воркера, только входные аргументы).
- `request_handler.hpp/.cpp` — `handle_stacktrace`,
  `increment_per_client_metric`.
- `server_handler.hpp/.cpp` — `handle_favicon` (был `const`; `static` сильнее —
  метод не только не меняет состояние, но и не имеет доступа к `this`).
- `trace_logger.hpp/.cpp` — `generate_trace_id`, `generate_span_id`,
  `generate_traceparent` (чистая обёртка над `RandomUtils`), поэтому вызываются
  они по-прежнему через объект (`tracer->generate_span_id()` в
  `common_utils.cpp`) — для статических методов это легально.
- `test_coverage_ext.cpp` — `acquire_connection` у локальных `NullPool` и
  `InvalidPool` (тестовые стабы пула).
- `test_db_live.cpp` — `run_wait`, `spawn_server`;
  `test_nats_live.cpp` — `run_wait`, `spawn` (породительная логика `ServerFixture`,
  состояние процесса она не трогает).

У `static`-методов с отдельным объявлением `static` пишется **только** в
заголовке (в определении повторять нельзя — это ошибка), `const` пишется в
обоих местах: ровно так фиксы и сделали.

После автоправок вручную вычищена вёрстка: фиксы не переразбивают строки, из-за
чего появились пять строк длиннее 80 колонок (`parse_request_data`,
`observe_db_query_outcome`, `setup_tracing`, `handle_crash_test`,
`acquire_connection`), сбитый отступ у `prepare_response_data`/`run_wait`/
`spawn`/`generate_traceparent` и лишний пробел перед `;` в `handle_favicon`.
Плюс `static [[nodiscard]]` приведено к `[[nodiscard]] static`.

## Каскад: ещё 8 находок

Форсировать `const`/`static` — это не одна итерация: повторный свип показал 8
новых находок, которые появились именно из-за этой правки (классический
каскад при расширении гейта):

- `readability-make-member-function-const` — `db_query_executor_oracle.cpp`
  (`release_conn`), `request_handler.{hpp,cpp}` (`record_and_maybe_reject_duplicate`,
  `reject_rate_limited`, `push_to_backend`): все три сами ничего не меняют, а
  вызываемые ими методы теперь `const`, поэтому и они могут стать `const`.
- `modernize-use-nodiscard` — `NatsPushService::push_request` и
  `ServerFixture::wait_ready`. Чек рассматривает только `const`-методы, так что
  «проявились» они ровно после `const`-правки. Оба возвращают то, что вызывающий
  обязан проверить (сериализованный envelope и признак готовности сервера), так
  что `[[nodiscard]]` уместен; все места вызова результат используют.
- `misc-const-correctness` (этот чек включён давно) — два локальных
  `NatsPushService push(ctx)` в `test_proxy_handlers.cpp` стали
  `const NatsPushService push(ctx)`: `push_request` теперь `const`, объект больше
  ни для чего не нужен неконстантным.

Заодно выправлен отступ объявления `record_and_maybe_reject_duplicate` в
`request_handler.hpp`: оно висело в нулевой колонке (правка всё равно рядом).

Итого правок раунда — **44 находки** (36 + 8 каскадных), все в проектных файлах.

## Что осознанно не сделано

- `readability-static-accessed-through-instance` (обращение к статическому
  методу через объект) **не** включён: как раз после этой правки у нас есть
  легальные вызовы вида `tracer->generate_span_id()`. Включать его — отдельный
  раунд с переделкой мест вызова (`JaegerLogger::generate_span_id()`), иначе
  два чека тянут в разные стороны.
- `readability-named-parameter` (9 находок) — не включаем: безымянные
  параметры в стабах/заглушках (`test_...`, сигнатуры-заглушки) идиоматичны.
- `readability-container-size-empty` — отклонён ещё в round 148 (56 находок,
  почти все — сравнение строки с `""` в тестах).

## Проверка

- `./rebuild-and-run.sh` — rc=0 (`All tests passed (2814 assertions in
  589 test cases)`).
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck (`./scripts/run-cppcheck.sh`) — чисто.
- clang-tidy `--all` (47 TU) с новым набором — ноль предупреждений в проектных
  файлах (`✓ clang-tidy: no errors or warnings in project files`).
- coverage (`./scripts/run-coverage.sh`) — тесты прошли (`coverage unit tests:
  109s, all passed`), production 6525/7138 = 91.41%, гейт сначала ругнулся на
  `metrics_history.hpp` (96.74% → 94.57%), и baseline переанкорен на **пол**:
  сэмплерный поток в `MetricsHistory` успевает то на 2 строки больше, то
  меньше, поэтому в baseline записано 87/92 (`--update` тронул только эту
  запись; у остальных файлов изменились лишь счётчики ветвей, которые гейт не
  сравнивает). В round 148 в baseline случайно попал удачный прогон (89/92) —
  откатили. Число «строк всего» не изменилось (7138 и 7138): правка
  `const`/`static` не выполнимых строк не добавляла и не убирала.

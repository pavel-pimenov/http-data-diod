# Round 69: покрытие юнит-тестами до 90% + гейт `--fail-under-line 90`

## Мотивация

Round 68 замерил покрытие и оставил его на уровне **85%** (12066/14165 строк) с гейтом
`--fail-under-line 0` в coverage-стадии `src/Dockerfile`, то есть фактически без контроля.
Задача раунда — довести line-coverage l2-proxy до цели **90%** и включить настоящий гейт,
чтобы откат покрытия валит CI, а не остаётся незамеченным.

## Изменения

### 1. `src/CMakeLists.txt` — тесты линкуют весь бинарник, а не его часть

Раньше `test_components` собирал только часть исходников: большая часть proxy-кода
(`request_handler`, `server_handler`, `nats_*`, `db_query_*`, ...) вообще не попадала в
отчёт покрытия, потому что не линковалась в тестовый бинарник. Теперь:

- в `test_components` добавлены все производственные TU, кроме `main.cpp`
  (там свой `main` — ловит Catch2): `proxy_init.cpp`, `request_handler.cpp`,
  `server_handler.cpp`, `l2_worker.cpp`, `l2_worker_nats.cpp`, `nats_client.cpp`,
  `nats_push_service.cpp`, `nats_poll_service.cpp`, `stats_logger.cpp`,
  `db_query_handler.cpp`, `db_query_executor_{oracle,postgres,factory}.cpp`,
  `odpi/embed/dpi.c`;
- подключены заголовки `odpi/include`, NATS и линковка `PostgreSQL::PostgreSQL`,
  `${NATS_C_LIBRARY}` (+ `stdc++exp` для `<format>`/экспериментальных библиотек);
- `set_target_properties(test_components PROPERTIES UNITY_BUILD OFF)` — тестовые TU
  живут в анонимных namespace-ах и unity-батчи из proxy-core/proxy-nats сталкиваются;
  coverage-стадия и так собирается без unity, но тесты должны быть unity-free в любой
  конфигурации.

Побочный эффект: отчёт покрытия теперь измеряет **бинарник целиком**, а не только то,
что тесты успели подключить раньше — знаменатель вырос с ~14165 до 14599 строк.

### 2. `src/shutdown_flag.cpp` + `src/main.cpp` — вынос глобального флага

`g_shutdown_flag` определялся в `main.cpp`. С подключением всех TU в тесты это определение
стало недоступным (в тестах свой `main`), поэтому оно вынесено в отдельный
`src/shutdown_flag.cpp`, а в `main.cpp` осталось `extern`. TU добавлен в обе цели.

### 3. `src/stats_logger.{hpp,cpp}` — период логирования как параметр конструктора

`600`с были захардкожены в `stats_logger.cpp` (4 использования: сообщение о старте,
`wait_for`, окно «Requests in last {}s»). Появился параметр конструктора

```cpp
static constexpr int kDefaultLogIntervalSeconds = 600;
StatsLogger(AppContext&, std::atomic<bool>&, int log_interval_seconds = kDefaultLogIntervalSeconds);
```

и член `m_log_interval_seconds`. Дефорт сохраняет поведение продакшена, а тесты гоняют
циклы с интервалом **1с** и вместо сна на 600с просто ждут ~1.6с. Позвончики
(`AppContext` и т.п.) не менялись — default-аргумент.

### 4. `src/test_l2_worker.cpp` (новый) — 4 кейса

`[l2-worker]`, 42 assertion'а:

- конфигурация/ready;
- **circuit breaker**: 5× на мёртвый backend → 503 → пустой список URL → 403 и
  `prepare_response_data`; отдельный `L2Worker` на этот блок, потому что пул
  HTTP-клиентов привязывается к host первого запроса, и следующий запрос одним
  инстансом уходил бы на мёртвый бэкенд и получал 404 вместо ожидаемого;
- **flaky-ретрай** во втором экземпляре `L2Worker`;
- доступ к внутренностям — `friend class L2WorkerTestAccess` в `l2_worker.hpp`.

### 5. `src/test_proxy_handlers.cpp` (новый) — 28 кейсов

Основной объём новых строк: ProxyInit (в т.ч. **per-IP коллектор** — `acquire()` +
`m_per_ip_metrics_collector->Collect()`), ServerHandler (включая **traced**-ветку),
RequestHandler (readiness, rate-limit 429, POST-пути, **DB-gateway** с ошибкой NATS →
503/DB_UNAVAILABLE, **traced**-варианты: round-trip span и proxy-span при timeout),
NatsPushService/NatsPollService/NatsClient (в т.ч. traceparent/inlet-span при
подключённом трейсере), DbQueryHandler + фабрика, **OracleQueryExecutor** (background
init без достижимого сервера → `!m_ready` → 503, `ping` → false, `refresh_pool_gauges`
через `set_pool_metrics`), **StatsLogger** (циклы в режимах proxy/worker/l2-server —
метрики инициализируются в `AppContext` безусловно, поэтому `collect_mode_stats`
работает в любом MODE), ResponseBuilder (binary payload → base64-декод в `hello`).

Тестовые фикстуры: `ProxyEnv` (fail-fast NATS: `NATS_ENABLE_TLS=true`,
`NATS_TLS_CA_CERT_FILE=/nonexistent-test-ca.pem`, таймауты 1с), `EnvGuard`
(initializer_list пар), `attach_tracer()` (JaegerLogger на `http://127.0.0.1:1`,
batch 10000/flush 100000/sample 1.0 — закрывает все `JaegerSpanLogger`-ветки без
живого Jaeger).

## Динамика покрытия

| Шаг | Строк покрыто / всего | % |
| --- | --- | --- |
| baseline (round 68) | 12066 / 14165 | 85.0% |
| линковка всего бинарника + новые тесты | 12917 / 14469 | 89.27% |
| + OracleQueryExecutor / StatsLogger / traced-ветки | 13184 / 14600 | 90.30% |
| **финальная сборка coverage-стадии** | **13182 / 14599** | **90.3%** |

Функции — 91.5%, ветки — 40.7% (веточный покрытие — отдельная тема, в раунд не входило).

## Гейт

`src/Dockerfile` (coverage-стадия): `--fail-under-line 90`. На время разработки в рабочей
копии стояло `0`, чтобы coverage-стадия не падала до достижения цели; перед коммитом
возвращено `90` и подтверждено полным прогоном `./scripts/run-coverage.sh`
(`=== coverage report rendered ===`, rc=0).

## Что осталось непокрытым и почему

Недостижимо без живых внешних сервисов (осознанно принимаем):

- `nats_client.cpp` connected-пути (~295 строк) — нужен брокер;
- `nats_poll_service.cpp` connected-пути — нужен брокер;
- `db_query_executor_postgres.cpp` живой-сервер пути (~245) — фабрика знает только
  `oracle`/`postgres`, mock-драйвера нет;
- `db_query_handler.cpp` ping/execute/ready — нужен живой executor;
- `request_handler.cpp` success-ветки DB-gateway (ответ от worker'а) и 504-ветка
  «worker не ответил» — обе требуют подключённого NATS.

Отдельно — **phantom-строки gcov**: первая/последняя строка многострочных выражений
(обычно `Logger::debug/info(...)`, continuation-инициализаторы) числится непокрытой, хотя
выполняется. Тестами не лечится, за ними гнаться бессмысленно.

## Как замерялось

gcovr на хост не ставится — отчёт считается в builder-контейнере:

```bash
docker run --rm -i -v "$PWD/src:/app" -v l2-cov-build:/app/build-cov -w /app \
  http-data-diod:builder bash -c 'ninja -C build-cov test_components test_proxy_core && \
  ./build-cov/test_components && ./build-cov/test_proxy_core && cd build-cov && \
  gcovr --object-directory . --root /app --filter "/app/.*\.(cpp|hpp|h)$" \
    --exclude "/app/prometheus-cpp/.*" --exclude "/app/httplib/.*" \
    --exclude "/app/base64/.*" --exclude "/app/json/.*" \
    --json -o cov.json --gcov-ignore-errors=all'
```

Нюансы:

- JSON gcovr: корень `{"files", "gcovr/format_version"}` (нет `summary`), у файла ключ
  `file` — путь относительно `/app`, а не basename;
- пересборка меняет checksum `.gcda` → ожидаемые `libgcov profiling error: overwriting ...
  different checksum`; для честного замера после сборки нужно **удалить все `.gcda`**
  и прогнать оба тестовых бинарника заново — накопленные данные от разных прогонов
  вводят в заблуждение (в частности, гонка без `gcovr` в том же контейнере даёт
  неизменённый старый `cov.json`);
- честная симуляция coverage-стадии (clean `.gcda` + оба бинарника) дала **13182/14600**,
  т.е. запас до гейта ~42 строки.

## Проверки

- `./rebuild-and-run.sh` — сборка и стек поднялись, дашборды Grafana/Perses синхронизированы 8/8;
- `./health-check.sh all` — rc=0;
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0 (нет потерь/скрещиваний);
- `./scripts/ci-gate.sh all` — **rc=0** (unit + offline + runtime: message counter, DB-gateway
  e2e 7/7, golden metrics 80/80, metric consistency 84/84, Grafana ↔ Perses parity 8/8);
- `./scripts/run-coverage.sh` — гейт `--fail-under-line 90` проходит;
- clang-tidy (`./scripts/run-clang-tidy.sh`) — чисто по всем изменённым файлам,
  включая новые тесты.

### Попутный фикс `scripts/run-clang-tidy.sh`

Скрипт на хосте падал на `nproc` (GNU-only, в macOS нет): `$jobs` оказывался пустым,
`xargs -P ''` внутри контейнера не запускал ни одного clang-tidy, а скрипт всё равно
печатал «no errors or warnings» — **тихий false-green**. Теперь
`nproc → sysctl -n hw.ncpu → 4`, и запуск без `CLANG_TIDY_JOBS` реально линтит
(`jobs=12` на рабочей машине).

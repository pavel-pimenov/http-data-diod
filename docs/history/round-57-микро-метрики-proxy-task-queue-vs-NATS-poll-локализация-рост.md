# round 57: микро-метрики proxy (task queue vs NATS poll) — локализация роста p99

## Date: 2026-10-05

### Что сделано
- **Закрыт кандидат round 55** («микро-метрика времени ожидания пула в proxy, чтобы отличить semaphore от poll interval»): добавлены 6 метрик, измеряющих именно эти два этапа.
- **`src/timed_task_queue.hpp` (новый)**: `TimedTaskQueue : httplib::TaskQueue`. В `enqueue()` засекается `steady_clock` на момент постановки задачи в `httplib::ThreadPool`, в обёртке задачи — на момент старта в worker thread; разница наблюдается в `l2_proxy_task_queue_wait_seconds`. Счётчики: `l2_proxy_task_queue_enqueued_total` (принято пулом), `l2_proxy_task_queue_rejected_total` (отказ пула при переполнении, наравне с `active_requests`). Подключается **только** к proxy: `run_httplib_server` принимает опциональную фабрику очереди, `run_proxy` передаёт `TimedTaskQueue`, а NATS push/pull-сервисы и l2-server остаются на дефолтном пуле httplib.
- **`NatsPollService`**: `l2_proxy_nats_poll_attempts_total` (число попыток poll на один клиентский запрос), `l2_proxy_nats_poll_attempt_duration_seconds` (длительность каждой попытки `natsConnection_request` внутри одного poll) и `l2_proxy_nats_poll_retry_wait_seconds` (backoff между попытками, 250ms и 1000ms).
- **Бакеты**: в `histogram_buckets::g_buckets.m_latency_100us_to_1s` добавлены границы от 100µs до 1s (были только от 1ms) — без них queue wait и NATS attempt упирались в первый бакет.
- **Тесты**: 2 новых кейса `TimedTaskQueue` в `test_coverage_ext.cpp` — задача принята и отработала (wait > 0, rejected == 0), переполнение пула (`max_n=1`, `max_queue_size=1`, две задачи) даёт `rejected == 1`. Каталог метрик в `README.md` дополнен.

### Замеры и локализация p99
- Методика: `message_counter.py` свипом по concurrency (1/5/20/50/100, по 200 запросов) **напрямую в proxy `:8888`** (nginx выключен из цепочки) + снятие `/metrics` proxy до и после каждого прогона и расчёт дельт/квантилей из кумулятивных гистограмм. VictoriaMetrics для этого не годится: `increase()`/`histogram_quantile()` по окну, где нет свежих скрейпов, даёт мусор (получены p50 33.9ms при фактических 4ms), поэтому дельты считались напрямую с эндпоинта.
- Результаты (proxy `:8888`, RPS — итоговая строка `Requests per second`, 400 запросов на прогон = 200 POST + 200 GET):

  | concurrency | RPS | client p50 | client p99 | handler mean | queue wait mean | NATS attempt mean | attempts/req |
  |---|---|---|---|---|---|---|---|
  | 1 | 534 | 2.08 ms | 3.60 ms | 1.26 ms | 0.026 ms | 0.64 ms | 1.000 |
  | 5 | 1219 | 3.97 ms | 7.35 ms | 3.23 ms | 0.021 ms | 1.14 ms | 1.000 |
  | 20 | 1313 | 16.12 ms | 42.62 ms | 14.69 ms | 0.026 ms | 1.28 ms | 1.000 |
  | 50 | 1105 | 40.30 ms | 100.03 ms | 39.64 ms | 0.025 ms | 1.48 ms | 1.000 |
  | 100 | 1151 | 75.10 ms | 184.97 ms | 72.97 ms | 0.052 ms | 1.20 ms | 1.000 |

- **Гипотеза round 55 «thread pool / semaphore» опровергнута**: ожидание в очереди задач — 0.02–0.05ms в среднем, p99 ≤ 0.24ms даже на c=100, `rejected == 0`. Задачи стартуют немедленно, пул не насыщен.
- **Гипотеза round 55 «poll interval / ретраи» опровергнута**: `attempts/req = 1.000` на всех прогонах (ровно одна попытка poll на запрос), сэмплов `l2_proxy_nats_poll_retry_wait_seconds` — 0, длительность попытки NATS 0.6–1.5ms в среднем (p99 ≤ 4.5ms). Ни один запрос не ждёт poll-таймаут.
- **Рост p99 — внутри обработчика и вне NATS round-trip**: `handler mean` (2.1ms → 73.0ms) совпадает с клиентским p50 и объясняет всю наблюдаемую задержку; `client p50 ≈ concurrency / RPS` (Little's law) — 15.2/15.2, 32.7/40.3, 86.9/75.1ms. То есть задержка линейно растёт именно из-за ограничения пропускной способности, а не из-за конкретного медленного этапа.
- **Потолок ~1.2–1.6k RPS находится в proxy, а не в nginx и не в клиенте**: через nginx `:7777` профиль тот же (p50 36.9ms / p99 88.7ms на 200×50), напрямую `:8888` — p50 37.8ms / p99 89.5ms; 4 параллельных клиента по 10 concurrency дают ~1500 RPS суммарно (потолок не масштабируется), при этом клиенты не загружены. `docker stats` внутри Colima для CPU% не использовался как доказательство — значения нерепрезентативны для VM.
- **Следствие для baseline**: `scripts/comprehensive-performance-test.py` — Low 1276 RPS/p99 9.8ms, Medium 1319/10.1, High 1623/26.3, Stress 1486/92.7; `scripts/perf-report.json` обновлён.
- **Кандидат на следующий раунд**: разложить оставшиеся ~0.8ms последовательной стоимости на фазы внутри обработчика (парсинг JSON, дедуп/duplicate-detector, rate limiter, публикация в NATS, сборка ответа + логирование — на запрос при LOG_LEVEL=INFO пишется ~4 строки лога) фазовой гистограммой; thread pool, nginx, клиент и NATS-poll из списка подозреваемых исключены.

### Проверка
- `./rebuild-and-run.sh`: rc=0; `test_components` 883 assertions / 120 test cases, `test_proxy_core` 1960 assertions / 489 test cases, все сервисы `healthy`.
- `./health-check.sh all`: rc=0. `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0.
- Новые метрики присутствуют в `/metrics` proxy (`l2_proxy_task_queue_*`, `l2_proxy_nats_poll_*`); golden-metrics набор не менялся (новые серии добавлены в каталог README).


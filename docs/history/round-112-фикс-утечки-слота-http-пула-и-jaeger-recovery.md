# Round 112: фикс утечки слота `HttpClientPool` при исключении в потребителе (+ Jaeger-recovery на настоящем сбое)

Закрываем дефект, найденный в round 111 при анализе ветки «Jaeger send
recovered». Это уже не покрытие, а исправление продакшн-логики пула HTTP-клиентов
с собственными регрессионными тестами.

## 1. Проблема

`HttpClientPool::acquire_connection()` увеличивает `m_total_clients` (и
`m_active_clients`) и отдаёт соединение потребителю. Уменьшение слота делает
только `release_connection()`, а единственный вызов на пути трассировки стоит на
**успешном** пути (`trace_logger.cpp:720`). Если потребитель бросил исключение
(`send_batch` → `post_no_response` при недоступном Jaeger), `unique_ptr<HttpClient>`
уничтожается без возврата слота — счётчик `m_total_clients` навсегда остаётся
завышенным.

Практическое следствие: одна серия отказов Jaeger (1 попытка + 3 ретрая = 4
соединения при `kJaegerPoolMaxSize = 3`) вычерпывает пул трассировки целиком; все
последующие `acquire_connection()` ждут `acquire_timeout` (3 c) и бросают
исключение — «breaker» перестаёт быть восстановимым даже после того, как Jaeger
вернулся. Именно поэтому в round 111 recovery-тест пришлось строить на
искусственном «подвисании» первого POST: честный сценарий «отказ → восстановление»
упирался в исчерпанный пул.

## 2. Фикс

Минимальный по API: сигнатуры пула (`acquire_connection`/`release_connection`) и
все потребители не меняются.

* `HttpClient` получил `set_pool_release_hook(std::function<void(bool leased)>)` и
  `set_pool_leased(bool)`. Деструктор вызывает хук с текущим флагом (в том же
  `try`, где уже стоял «must not throw»).
* Пул ставит хук один раз при создании клиента. Хук срабатывает, только если
  клиент уничтожен **в арендованном** состоянии (`leased == true`): тогда он
  возвращает слот — `m_total_clients--`, `m_active_clients--` и обновляет
  Gauge `active`. Праздные соединения (в очереди) по-прежнему учитываются
  явными ветками пула (stale-эвикция / invalid / pool-full), поэтому хук для них
  молчит и двойного вычитания нет.
* Флаг аренды: `true` при создании и при выдаче из очереди
  (`try_acquire_from_queue`), `false` в `release_connection` (соединение снова под
  контролем пула) и неявно у праздных.
* Счётчики переведены в `std::shared_ptr<Counters>`: хук захватывает shared_ptr по
  значению, так что даже если клиент переживёт пул, учёт останется валидным и
  хук не тронет уже уничтоженный объект пула.

Итог: серия отказов Jaeger больше не вычерпывает пул трассировки навсегда,
breaker снова восстановим, а `active`-гейдж корректно откатывается при потере
соединения.

## 3. Тесты

**`test_http_pipeline.cpp` (+2 кейса, `[http-client-pool]`):**

* «connection dropped without release returns its slot» — клиент уничтожается в
  блоке без `release_connection` (эмуляция бросившего потребителя): до фикса
  `total_clients()==1` и следующий `acquire_connection()` таймаутит; после фикса
  `total_clients() == 0`, `active_clients() == 0` и следующий acquire берёт новый
  слот, который затем штатно возвращается.
* «dropped connection resets the active gauge» — то же через метрики: после
  выхода из блока `active.Value() == 0`.

**`test_trace_logger.cpp` — честный e2e-recovery вместо искусственного подвисания.**
Round 111-тест с режимом мок-сервера `hang_requests` заменён: у мок-сервера
убраны `hang_requests`/`m_hung`/`m_stopping`, а вместо них появился явный порт
(`TraceMockServer(slow_us, port)`, `port = 0` → эфемерный). Новый сценарий:

1. занять loopback-порт и отпустить его → endpoint мёртв;
2. первый батч честно падает (connection refused), ждём `failed() >= 1` — это
   гарантирует, что все ретраи уже отработали и сервер не «подхватит» их;
3. поднять `TraceMockServer` на том же порту;
4. второй батч доставляется: `sent() >= 1` и `body_count() == 1`, при этом на
   успешном пути исполняется ветка «Jaeger send recovered» (722-725).

Тест — настоящий регрессионный: со старым кодом три упавшие попытки вычерпывают
пул (max_size=3), второй батч уходит в acquire-timeout по 3 c на ретрай и
`sent()` остаётся 0, т.е. REQUIRE падает. Теперь ветка recovery покрывается
реальным сбоем, а не искусственным таймаутом.

## 4. Итог

| метрика | было | стало |
|---|---|---|
| production строки | 6496/7152 = 90.83% | **6515/7172 = 90.84%** |
| production ветви | 7143/13499 = 52.92% | **7159/13527 = 52.92%** |

Приросты по файлам (baseline переанкорен, 78 файлов, только плюсы):
`http_client_pool.cpp` 105/117 → 118/131 строк (+13), ветви 74/126 → 83/142 (+9);
`http_client.cpp` 92/98 → 97/103 (+5), ветви 80/136 → 83/140 (+3);
`http_client_pool.hpp` ветви 17/35 → 21/43 (+4); `http_client.hpp` +1 строка.

Тесты: 2736 assertions / 578 cases → **2755 assertions / 580 cases**.

## 5. Проверка

* `./scripts/run-coverage.sh` — EXIT=0, оба бинаря «All tests passed», скипов нет,
  гейты (`--fail-under-line 90`, `--fail-under-branch 40`, production
  `--fail-under-branch 50`) пройдены, «OK: регрессий покрытия нет».
* `./scripts/run-cppcheck.sh` — EXIT=0 (изменённые 6 файлов чисты).
* `./scripts/run-clang-tidy.sh` — EXIT=0 (только унаследованные `misc-include-cleaner`
  в `http_client.hpp`/`http_client_pool.hpp`).
* `./rebuild-and-run.sh` — EXIT=0, все обязательные health-check прошли,
  `glitchtip` unhealthy как и раньше (не обязательный), Perses 8/8.
* `./health-check.sh all` — rc=0; `message_counter.py --iterations 1 --concurrent 1`
  — rc=0.

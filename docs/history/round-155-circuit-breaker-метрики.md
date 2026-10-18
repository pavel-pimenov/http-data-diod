# round 155 — метрики circuit breaker: переходы и отказы

## Что сделано

Первый пункт P1 из `docs/TODO-metrika-kandidaty.md`: у `CircuitBreaker` наружу
выходило только состояние (`l2_worker_circuit_breaker_state`), внутренние счётчики
(`m_failure_count`/`m_success_count`) не экспортировались — по дашборду нельзя было
увидеть, как часто брейкер «щёлкает» и сколько запросов он отбрасывает в OPEN.

Добавлены две метрики:

| Метрика | Тип | Метки | Смысл |
|---|---|---|---|
| `l2_worker_circuit_breaker_transitions_total` | counter | `to` (open/half_open/closed) | Переходы состояния брейкера по целевому состоянию |
| `l2_worker_circuit_breaker_rejected_total` | counter | — | Запросы, отброшенные открытым брейкером (fast-fail) |

### Точки записи

`src/circuit_breaker.hpp`/`.cpp`: у брейкера появились два опциональных хука
(`m_transitions`, `m_rejected`) и `set_counters(...)` по образцу существующего
`set_gauge(...)`; приёмники опциональны, поэтому тесты, которые гоняют только
машину состояний, работают без реестра. Приватный `record_transition(const char *to)`
инкрементирует серию целевого состояния из уже существующих точек перехода:

- `transition_to_open()` → `to="open"` (закрывает и `CLOSED → OPEN`, и
  `HALF_OPEN → OPEN` — целевое состояние одно и то же);
- ветка истечения open-окна в `allow_request()` → `to="half_open"`;
- достижение порога успехов в `record_success()` → `to="closed"`.

Счётчик отказов инкрементируется в `allow_request()` там же, где возвращается
`false` (OPEN и окно ещё не истекло) — то есть ровно на пути fast-fail, который
в `src/l2_worker.cpp` приводит к отказу запроса.

`src/l2_worker.cpp`: после `set_gauge(...)` добавлен `set_counters(...)` с
`s->m_circuit_breaker_transitions_total` и `s->m_circuit_breaker_rejected_total`.

`src/app_context.hpp`/`.cpp`: два новых члена `WorkerMetrics` + регистрация рядом
с `l2_worker_circuit_breaker_state`.

### Почему серии `to` предсоздаются

prometheus-cpp отдаёт лейбл-семейство в `/metrics` только после первого `Add()`, а в
здоровом стеке брейкер может ни разу не сработать. Тогда
`l2_worker_circuit_breaker_transitions_total` отсутствовал бы в экспозиции, и
golden check (в CI гоняется с `--all`) валился бы, хотя метрика зарегистрирована.
Поэтому `init_worker_metrics()` после конструирования `WorkerMetrics` заранее
материализует три серии (`to="open"/"half_open"/"closed"`) значением 0 — они видны
с первого скрейпа, а переходы только инкрементируют готовые серии. Счётчик отказов —
обычный singleton-counter, он всегда есть в экспозиции как 0.

### Четыре источника

- C++-регистрация — `src/app_context.cpp`;
- PromQL — две новые панели 51/52 в `create_worker_dashboard()`
  (`scripts/generate-grafana-dashboards.py`), строка «Errors & Resilience»:
  переходы по `to` (rate 5m, sum by `to`) и rate отказов (порог green/yellow/red);
- README — две строки в таблице `l2-worker` каталога + отметка `set_counters` в
  таблицах покрытия `circuit_breaker.cpp`;
- `scripts/metrics-golden-check.py` — оба имени в `CATALOG`.

Perses-дашборды конвертируются из Grafana-фабрик, поэтому панели там появились
автоматически — parity-check это подтвердил.

## Тест

В `src/test_components.cpp` добавлен кейс «CircuitBreaker: transition and
rejection counters»: он прогоняет `CLOSED → OPEN` (счётчик `open`=1), fast-fail
(`rejected`=1), `OPEN → HALF_OPEN` (окно истечения симулируется записью
`steady_us() - g_open_timeout_us - 1` в `m_last_failure_time_us`, без реального
сна), `HALF_OPEN → OPEN` (снова `open`=2) и `HALF_OPEN → CLOSED` (`closed`=1).
Это закрывает все три ветви `record_transition`, включая `half_open`, которую
иначе не покрыть без 10-секундного ожидания.

## Проверка

- `./rebuild-and-run.sh` — rc=0, unit tests `All tests passed (2822 assertions in
  590 test cases)` (было 2814/589), health зелёный.
- `./health-check.sh all` — rc=0; `python3 message_counter.py --iterations 1
  --concurrent 1` — rc=0.
- `./scripts/ci-gate.sh runtime` — зелёный: golden metrics `82/82 families,
  WORKER=25`, metric-name consistency `86 metrics` (C++/dashboards/Perses/README/
  golden + runtime `/metrics`), Grafana↔Perses parity 8/8.
- Живой `/metrics` воркера (19091) отдаёт обе метрики, три серии `to` видны с 0.
- cppcheck — чисто. clang-tidy `--all` (47 TU) — ноль предупреждений.
- coverage (`./scripts/run-coverage.sh`) — rc=0, `all passed`, production
  `6551/7164 = 91.44%` (было 6525/7138 = 91.41%), `OK: регрессий покрытия нет`
  (переанкор не потребовался).

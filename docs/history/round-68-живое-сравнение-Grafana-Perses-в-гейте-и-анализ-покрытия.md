# Round 68: живое сравнение Grafana ↔ Perses в гейте + анализ покрытия юнит-тестами

## Мотивация

Round 66–67 поставили Perses рядом с Grafana и закрепили статический паритет
(offline-проверка конвертера, сверка имён метрик пяти источников). Но:

- **живые инстансы не сверялись**: offline-гейт проверяет *определения*, а не то, что
  реально лежит в Grafana (provisioned API) и в Perses (file-БД после PUT-синка). Дрейф
  (панель не пересоздалась, сеттинг сломался, переменная не переехала) гейт не ловил;
- самое уязвимое место — **PromQL на живых данных**: проверить, что обе системы отдают
  одни и те же series и значения, раньше можно было только руками через curl;
- отдельно стояла задача **посмотреть на реальное покрытие юнит-тестами** и понять,
  какая часть кодовой базы вообще измеряется.

## Изменения

### 1. `scripts/compare-grafana-perses.py` — живое сравнение двух систем

Скрипт (stdlib-only, как и остальные `scripts/*.py`) сверяет запущенные инстансы в
двух режимах:

**Структура** (по каждому из 8 дашбордов):

- набор дашбордов (Grafana uid ↔ Perses name);
- число панелей — Grafana-`row` разворачиваются во вложенные панели, Perses-панели
  берутся из `spec.panels` (dict);
- **множество PromQL-запросов** после нормализации `${vm:regex}` / `${vm}` → `$vm`
  (Grafana и Perses пишут переменную по-разному, остальной текст — идентичен);
- заголовки row-секций Grafana ↔ заголовки `layouts[].spec.display.title` Perses
  (Grid-секции — это perses-эквивалент row-групп).

Извлечение запросов учитывает реальные схемы API: Grafana — `panels[].targets[].expr`
(с рекурсией во вложенные панели row), Perses —
`panels[].spec.queries[].spec.plugin.spec.query`.

**Данные**: выборка PromQL с каждой доски (`up` + 4 выражения, равномерная выборка
из отсортированного списка) выполняется через оба API:

- Grafana: `POST /api/ds/query` (мгновенный запрос, датасорс uid `prometheus`);
- Perses: `GET /proxy/globaldatasources/prometheus/api/v1/query`
  (**множественное число** `globaldatasources` — singular и `/api/proxy/...` дают
  404 и падают в static-хендлер).

Значение переменной `$vm` резолвится из живого `count by (vm) (up)`. Ключ серии —
множество лейблов (канонизированный tuple), чтобы скалярные вырожденные результаты
Grafana (`Value`) и Perses (`value`) не давали ложного DIFF. Сравнение значений —
с относительным допуском `1e-3`: два запроса выполняются с интервалом в миллисекунды,
и `rate()` по счётчику за окно 5m между ними уже слегка отличается. Пустые на обеих
сторонах семплы считаются отдельно (`empty on both sides`) — это вакуумное совпадение
(на тестовом стеке 7 из 39), поэтому при нуле непустых результатов скрипт падает.

Режимы и интеграция:

```bash
python3 scripts/compare-grafana-perses.py            # структура + живые данные
python3 scripts/compare-grafana-perses.py --no-live   # только структура
python3 scripts/compare-grafana-perses.py --allow-down # rc=0, если UI недоступны
```

Env: `GRAFANA_URL` / `GRAFANA_USER` / `GRAFANA_PASSWORD` / `PERSES_URL`.
Коды возврата: 0 — паритет, 1 — расхождение, 2 — сервис недоступен.

### 2. `scripts/ci-gate.sh` — чек в runtime-гейте

В `g_runtime()` после metric-consistency добавлен запуск
`compare-grafana-perses.py --allow-down`: контейнеры Grafana/Perses упали — чек
пропускается с предупреждением (rc=0, как у остальных runtime-чеков), расхождение —
падение гейта. Header-комментарий `ci-gate.sh` дополнен (usage `runtime` теперь
упоминает Grafana ↔ Perses parity).

### 3. `README.md` — раздел Perses дополнен

В блок команд добавлены `compare-grafana-perses.py` (`--no-live`) и абзац, что именно
скрипт сверяет и что он висит в runtime-гейте.

## Результаты живого сравнения

```
structural comparison: Grafana=8 Perses=8 dashboards
  structure (panels/exprs/sections) matches for all 8 dashboards
data comparison (vm=MacBook-67CF):
  data samples compared: 39 (empty on both sides: 7)
OK: Grafana and Perses dashboards are in parity
```

- дашбордов 8/8 (`l2-worker, l2-proxy, l2-server, nats-dashboard, nginx-metrics,
  l2-slo-tracking, l2-sentry-delivery, l2-distributed-tracing`);
- панелей 122/122 (Grafana без учёта 39 row-обёрток);
- PromQL-выражений 171 на каждой стороне — множества идентичны после нормализации
  переменной;
- row/grid-секций 39/39 с одинаковыми заголовками (row-группы переехали в Perses
  через `layouts`/`Grid`);
- 39 живых семплов идентичны по лейблам и значениям (7 пусты на обеих сторонах —
  фиксированные окна `rate[1m]`/`rate[5m]` без роста счётчика, см. замечание в README).

Попутно отлажены три обманчивых «тихих» бага первой версии скрипта (все давали
зелёный свет без единой реальной проверки): извлечение `panel["expr"]` вместо
`targets[].expr` (0 выражений на обеих сторонах → сравнение множеств пусто==пусто),
фильтр нерезолвленных переменных **до** подстановки `$vm` (отбрасывал 166 из 171
выражения) и ключ скалярной серии `Value`↔`value`. Поэтому в скрипте есть явные
счётчики `panels/exprs/sections`, `data samples compared` и падение при нуле
непустых семплов — пустой прогон не должен выглядеть как успех.

## Анализ покрытия юнит-тестами (`./scripts/run-coverage.sh`)

Прогон: 1965 assertions / 490 tests passed, гейт `--fail-under-line 90`
(`src/Dockerfile:215`) пройден:

| Метрика | Значение |
| --- | --- |
| Lines | **97.9%** (10535 / 10761) |
| Functions | 93.7% (1323 / 1412) |
| Branches | 41.5% (21158 / 50998) |
| Файлов в отчёте | 66 |
| Файлов < 90% | 1 (`http_client_pool.cpp` — 89.7%, 105/117, missed 12) |
| Файлов < 75% | 0 |
| Медиана по файлам | 98.9% |

Остальные низкие: `trace_logger.cpp` 90.7% (missed 40), `l2_routing.hpp` 91.1%,
`thread_pool_wrapper.hpp` 92.3%.

### Главная находка: 97.9% — покрытие измеряемой подмножества

Coverage-таргет собирает только `test_components` + `test_proxy_core`
(`src/CMakeLists.txt:512`, `:584`, stage `coverage` в `src/Dockerfile`), поэтому в
отчёт попадают лишь 66 файлов. **37 файлов `src/` (7403 строки, из них 5364 строки
`.cpp`) в отчёт не попадают вообще** — их кодовая база не компилируется в тесты и
покрытие для них фактически 0%:

```
 886 request_handler.cpp        508 main.cpp
 777 nats_client.cpp            453 db_query_executor_postgres.cpp
 676 l2_worker_nats.cpp         238 nats_poll_service.cpp
 588 l2_worker.cpp              187 server_handler.cpp
 579 db_query_executor_oracle.cpp 156 proxy_init.cpp
                                 129 stats_logger.cpp
                                 119 db_query_handler.cpp
                                  56 nats_push_service.cpp
                                  16 db_query_executor_factory.cpp
```

То есть под гейт ≥90% подпадает только обвязка, которую тесты реально поднимают;
«сердце» l2-proxy (обработка запросов, NATS-клиент, воркеры, DB-экзекьюторы,
`main`) — вне замера. Оценка по всем `.cpp`: ~10535 покрытых из ~16100 строк
≈ **65%** против заявленных 97.9%.

Следующие шаги (не делались в этом раунде): подключить горячие неизмеряемые файлы
к тест-таргетам (или собрать отдельный coverage-таргет по всему `l2-proxy`) и
поднять порог по веткам (41.5% при гейте только по строкам).

## Проверки

- `./scripts/ci-gate.sh all` — rc=0 (unit 490/1965, offline: lint-python,
  Perses `--check`, metric-consistency 84/84, env-var, docker-context; runtime:
  message counter, DB gateway 7/7, golden 80/80, metric-consistency runtime,
  **Grafana ↔ Perses parity** — новый чек);
- `./rebuild-and-run.sh` — rc=0, все сервисы healthy, синк Perses 8/8;
- `./health-check.sh all` — все эндпоинты OK;
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0, потерь и
  скрещённых ответов нет;
- `python3 scripts/lint-python.py` — 0 issues.

# Round 109: покрытие детерминированных блоков без брокера (routing, LRU/TTL, пулы)

Продолжение линии round 108: целимся только в **реально достижимые** блоки
(полные ветки/строки, которые исполняются детерминированно), а не в артефакты
gcov. В отличие от раунда 108, здесь всё накрыто **без живого брокера и без
Postgres** — чистые заголовочные компоненты, поэтому тесты быстрые и не флейкуют.

Раунд дал **+15 покрытых строк** (прогноз был +12…16): 6471/7152 → 6486/7152,
90.48% → **90.69%**; ветви 7108/13499 → 7129/13499, **52.66% → 52.81%**.

## 1. `l2_routing.hpp` — legacy-prefix fallback при непарсящемся base (+5)

`find_allowed_l2_server()` в блоке `try` использует `parse_url(allowed_base)`;
для base без схемы/хоста (`"/api"`) или с обрезанной схемой (`"http://"`)
парсер бросает исключение, и до этого не было покрыто ни `catch`, ни
legacy-fallback (строки 122-129).

Новые тесты в `src/test_proxy_core.cpp` (`[l2-routing]`):

* `path-only base falls back to the legacy prefix match` — `urls={"/api"}`, путь
  `/api/v1`: парсер падает → `catch` → `Logger::debug` → `normalized_path.rfind(
  allowed_base, 0) == 0` → true, `selected_url = "/api"`; проверяется и промах
  (`"/nope"` → `false`);
* `unparsable base without a prefix match is denied` — `{"http://"}`: `parse_url`
  падает («no host after protocol»), fallback не совпадает → идём дальше → отказ.

Итог: покрыты 124-127 и 129 (строка 122 — первая строка многострочного
`Logger::debug`, остаётся артефактом, как и в прошлых раундах).

Осознанно НЕ проверяется более слабое поведение fallback (raw-prefix без
границы сегмента: `"/apiv2"` тоже матчится `"/api"`) — это особенность
запасного пути для заведомо некорректного конфига, и закреплять её тестом
нежелательно.

## 2. `dynamic_labeled_family.hpp` — Gauge без провайдера и TTL-эвикция (+5)

В `Collect()` ветка `Gauge && !provider` вызывает `evict_stale_and_trim()`
(строка 91), а сама эвикция по TTL (160-163) не исполнялась: все Gauge-тесты
передавали провайдер, а Counter/Histogram-тесты не переживали TTL.

Новые тесты в `src/test_proxy_core.cpp` (`[labeled-family]`):

* gauge без провайдера: `get("alice",0)->Set(3.0)` → `Collect()` отдаёт серию
  (покрывает 91);
* `Counter` c `ttl_seconds = 1`: после `Increment()` и паузы 1.1 с
  `Collect()` пуст — метка эвиктится (покрывает 160-163).

Пауза 1.1 с — единственный способ добраться до сравнения по `steady_clock`
(инъекции времени в классе нет); тайминги уже используются в соседних тестах.

## 3. `dedup_cache.hpp` — реклейм протухших записей в `store()` (+2)

`store()` вызывает `evict_expired_locked()` до вставки, поэтому протухшая
запись удаляется (104-105) даже если между вызовами не было `find()`. Прежний
тест «entry expires after TTL» покрывал только проверку в `find()`.

Новый тест `DedupCache: store reaps expired entries before inserting`
(`[dedup-cache]`, `src/test_components.cpp`): TTL 20 мс, пауза 60 мс, второй
`store()` → первая запись исчезает.

Строка 111 (`early return` в `evict_oldest_locked`) остаётся непокрытой —
недостижима: функция зовётся только когда `m_entries.size() >= max_entries ≥ 1`,
и порядок (`m_order`) непуст по инварианту.

## 4. `pool_executor.hpp` — пул отдал `nullptr` (+1)

`execute_http_command_with_status()` при `acquire_connection() == nullptr`
бросает `std::runtime_error` (23-24); покрыт был только случай `pool == nullptr`.

Новый тест `Pool executor: pool returning no connection throws`
(`[pool-executor]`, `src/test_coverage_ext.cpp`) заводит мок-пул
(`client_type = MockHttpClient`), чей `acquire_connection()` возвращает
`nullptr`, и проверяет исключение.

## 5. `thread_pool_wrapper.hpp` — неизвестный тип пула (+2)

`enqueue()` имеет `default: throw std::runtime_error("Unknown thread pool type")`
(46-47), недостижимый через штатные значения `CUSTOM`/`NONE`.

Новый тест `ThreadPoolWrapper: unknown pool type is rejected on enqueue`
(`[thread-pool-wrapper]`) конструирует обёртку с `static_cast<Type>(42)` и
проверяет, что `enqueue()` бросает, а `queue_size()` безопасно возвращает 0
(поле пула пустое).

## 6. Что осознанно НЕ бралось

* `metrics_history.hpp:192-193` (`default: return 0.0` в приватном
  `extract_value`) — вызывается только с типами из `Registry::Collect`,
  недостижимо через публичный API;
* `dedup_cache.hpp:111` — см. §3;
* `http_client_pool.cpp` (12 строк: 26/60/70/90/124/138/163/176/182 — первые
  строки многострочных `Logger::*`, 144-146 — сброс счётчиков для
  «невалидного соединения в пуле», достижимого лишь при гонке с
  инвалидацией);
* `l2_worker.cpp` 54-55/76/97/135-136/158-163/202-206/288/384/407/439-444/465/
  525/554/560/577/596/600 — живые пути воркера (нужен NATS+HTTP/DB), плюс
  закрывающие скобки/`catch(...)` в деструкторах;
* `trace_logger.cpp` (40 строк: Sentry-breaker 426-446, Jaeger 723-738/757-771)
  — кандидат на следующий раунд, но требует аккуратной работы с брейкером.

## 7. Итог

| файл | строки | ветви |
|---|---|---|
| `dedup_cache.hpp` | 50/53 → 52/53 | 40/64 → 43/64 |
| `dynamic_labeled_family.hpp` | 87/93 → 92/93 | 85/164 → 91/164 |
| `l2_routing.hpp` | 72/79 → 77/79 | 91/142 → 99/142 |
| `pool_executor.hpp` | 17/18 → 18/18 | 16/30 → 18/30 |
| `thread_pool_wrapper.hpp` | 24/26 → 26/26 | 11/22 → 14/22 |
| **production всего** | **6471/7152 → 6486/7152 = 90.48% → 90.69%** | **7108/13499 → 7129/13499 = 52.66% → 52.81%** |

* `docs/coverage-baseline.json` переанкорен (`coverage-regression-check.py
  --update`), гейт — «OK: регрессий покрытия нет»;
* тест-кейсов: test_components 570 → 573, test_proxy_core 120 → 124 (+7).

## Проверка

* `./scripts/run-coverage.sh` — второй прогон rc=0: оба бинаря зелёные
  (`All tests passed (2710 assertions in 573 test cases)`,
  `892 assertions in 124 test cases`), гейты строки/ветвей пройдены,
  per-file гейт — «OK: регрессий покрытия нет»;
* **важно про первый прогон:** он упал на branch-гейте (38.8% < 40%):
  в coverage-стадии 12 live-DB тестов (`[db-live]` + `[db-live-combo]`)
  записались как SKIPPED («postgres server not available in this
  environment»), из-за чего покрытие ветвей просело ниже порога. В
  builder-стадии ТОГО ЖЕ билда те же тесты прошли без скипов, и повторный
  прогон coverage-стадии тоже прошёл без скипов. **Настоящая причина
  выяснилась на следующем шаге:** хост-диск был заполнен на 100%, и
  `initdb`/`mkdtemp` в покрытийном контейнере не могли создать datadir
  (`no space left on device`); тот же симптом уже фиксировался в round 91
  («12 пропущенных live-Postgres-тестов при диске 100%»). Диск очищен
  (`docker builder prune` + удаление dangling-образов, 0 → 8.5 ГБ свободно),
  после чего и покрытийный прогон, и синк Perses-дашбордов пошли зелёными.
  Отсюда же следствие: у провала был замаскированный вид — харнесс части
  веток `start()` (в т.ч. падение `initdb`) молчит, стоит добавить
  error-логирование причины, чтобы диск-фулл диагностировался сразу;
* `./rebuild-and-run.sh` + `python3 message_counter.py --iterations 1
  --concurrent 1` — зелёные (см. коммит). Первый запуск упал не из-за
  кода: диск был заполнен, поэтому Perses не смог распаковать плагины
  (`no space left on device`) и синк 8 дашбордов вернул 400; после очистки
  диска — `Perses dashboard sync complete: 8/8 successful`;
* pre-commit (cppcheck + clang-tidy по изменённым TU) — чисто.

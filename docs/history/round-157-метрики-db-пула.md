# round 157 — метрики сатурации пула PostgreSQL (P1 #2 из TODO)

## Что сделано

Второй пункт P1-очереди (`docs/TODO-metrika-kandidaty.md`): у DB Gateway был виден только
`l2_worker_db_pool_connections{db,state}` (idle/active), и по нему нельзя было ответить на
вопросы «насколько мы близко к лимиту» и «сколько стоит получить соединение». Добавлены три
метрики, все с меткой `db` (как у соседних `db_pool_connections`/`db_requests_total` —
несколько БД в конфиге не должны сливаться):

| Метрика | Тип | Метки | Что даёт |
|---|---|---|---|
| `l2_worker_db_pool_capacity` | gauge | `db` | настроенный `POOL_MAX` — знаменатель сатурации |
| `l2_worker_db_pool_exhausted_total` | counter | `db` | сколько acquire отброшено: все соединения заняты |
| `l2_worker_db_pool_acquire_duration_seconds` | histogram | `db` | цена получения соединения (из пула или новое подключение) |

### `waiting_requests` из TODO не реализован — и это осознанно

Пункт 2 плана предполагал gauge `l2_worker_db_pool_waiting_requests` («сколько запросов ждут
соединение»). При реализации выяснилось, что ждать в этом пуле нечего: `acquire_conn()` —
неблокирующий try-acquire, при исчерпании лимита он сразу возвращает `nullopt`, и запрос
получает 503 `DB_UNAVAILABLE` (это даже описано в README как свойство контракта: «запросы не
вешаются в очередь»). Gauge очереди ожидания был бы вечным нулём — то есть шумом, который
невозможно отличить от «всё хорошо». Роль этого сигнала выполняет пара
`active`/`capacity` (видно, сколько занято из лимита) плюс всплеск `exhausted_total`
(сколько раз лимит реально упёрся). В `docs/TODO-metrika-kandidaty.md` пункт помечен
реализованным, а `waiting_requests` — отклонённым с этой причиной.

### Как прокинуты метрики

Проводка повторяет существующий путь пул-гауджей:

- `src/app_context.hpp/.cpp` — три новых семейства в `WorkerMetrics` и их регистрация в
  `init_worker_metrics()` рядом с `l2_worker_db_pool_connections`;
- `src/db_query_executor.hpp` — в интерфейс добавлен
  `set_pool_saturation_metrics(capacity, exhausted, acquire_duration)` (чисто виртуальный, как
  соседний `set_pool_metrics`): Oracle-исполнитель и любой другой наследник `DbExecutorBase`
  получают реализацию базы (для Oracle она просто ничего не публикует — его обсервабилити вне
  области проекта), а единственный прямой наследник интерфейса — тестовый fake — получил
  пустой override в тестовом файле. Пустая реализация по умолчанию в заголовке была первым
  вариантом и не прошла пер-файловый гейт покрытия (см. «Чему учит»);
- `src/db_query_executor_base.hpp/.cpp` — override, хранение указателей и публикация:
  `set_pool_saturation_metrics()` **предсоздаёт** серии (`Add({{"db", name}})` + `Set(pool_max)`,
  ноль для счётчика и гистограммы), потому что prometheus-cpp отдаёт лейбл-семейство только
  после первого `Add()` (тот же урок, что в round 155 для переходов брейкера); дальше
  `publish_pool_exhausted()` и `observe_pool_acquire(seconds)`;
- `src/db_query_executor_postgres.cpp` — `acquire_conn()` стал тонкой обёрткой над
  `try_acquire_conn()`: замер времени вокруг всей попытки и `publish_pool_exhausted()` в ветке
  `m_total >= pool_max` (гистограмма получает и «пул выжат за 0 мс», и «создавали соединение
  10 с» — обе стороны одной проблемы);
- `src/db_query_handler.hpp/.cpp` + `src/l2_worker_nats.cpp` — три указателя доезжают от
  режимного реестра до каждого исполнителя в `init()`.

Метки `db` в новых семействах — отступление от плана («без меток»), зафиксировано в TODO.

### Источники метрик

Четыре источника синхронизированы: регистрация в C++, панели в
`scripts/generate-grafana-dashboards.py` (в панель 47 «DB пул соединений» добавлен таргет
`capacity`, новые панели 53 «DB пул: отказы из-за исчерпания» и 54 «DB пул: время получения
соединения» с p50/p95), каталог README и список `scripts/metrics-golden-check.py`. Perses-дашборды
генерируются из Grafana-описания, поэтому панели появляются и там (parity-гейт проверяет).

### Тесты

- `test_db_executor_base.cpp`: новый кейс «pool saturation metrics are published» — проверяет
  предсоздание серий (capacity = `pool_max`, нулевые счётчик и гистограмма до первого acquire),
  инкремент `publish_pool_exhausted()` и `sample_sum` гистограммы; второй кейс — что три
  nullptr-я указателя ничего не ломают.
- `test_db_live.cpp` (живой postgres в builder-образе): расширен кейс «pool exhaustion returns
  503» — `capacity == 1`, `exhausted_total == 1` и счётчик гистограммы ≥ 2 на реальном
  исчерпании пула `pool_max=1`; в «routes live postgres query and ping» добавлена проводка
  handler → executor и проверка, что capacity/ping-гистограмма заполняются через handler.
- В `test_metrics_support.hpp` добавлены `make_histogram_family()` и `histogram_series()`
  (у `Family<Histogram>::Add` нет перегрузки без бакетов — тесты, которые только читают серию,
  созданную продакшн-кодом, передают любые бакеты).

## Проверка

- `scripts/metrics-consistency-check.py --offline` и `--runtime` — `registered=89
  dashboards=89 perses=89 readme=89 golden=89`, `OK` (было 86).
- `scripts/generate-perses-dashboards.py --check` — `Offline check passed (8/8)`,
  у `l2-worker` стало 28 панелей (было 26).
- `./rebuild-and-run.sh` — rc=0, юнит-тесты `All tests passed (2837 assertions in 592 test
  cases)` + `(945 assertions in 125 test cases)` (было 2822/590: +2 кейса в
  `test_db_executor_base.cpp` и +15 проверок там же и в живых DB-тестах).
- `./health-check.sh all` — rc=0; `python3 message_counter.py --iterations 1 --concurrent 1`
  — rc=0.
- `./scripts/ci-gate.sh runtime` (со **включённым** профилем `databases`, то есть с поднятым
  postgres: иначе DB-часть гейта скипается) — rc=0, включая `DB gateway E2E: 7/7`,
  `OK: golden metrics set complete (85/85 families, … WORKER=28)` (до раунда было 25, то
  есть три новых семейства реально попали в VictoriaMetrics), consistency 89/89 и
  parity `Grafana=8 Perses=8`.
- Живой `/metrics` воркера (`:19091`) после инициализации экзекьютора:
  `l2_worker_db_pool_capacity{db="postgres"} 5`,
  `l2_worker_db_pool_exhausted_total{db="postgres"} 0` и полная сетка бакетов
  `l2_worker_db_pool_acquire_duration_seconds_bucket{…,le="0.001"…"10","+Inf"}` — то есть
  семейства экспортируются ещё до первого запроса (предсоздание серий работает).
- Сатурация проверена нагрузкой: `scripts/db-gateway-e2e-test.py --parallel 25` против
  `POOL_MAX=5` даёт `l2_worker_db_pool_exhausted_total 11` и
  `acquire_duration_seconds_count 52, sum 0.0466 s` (микросекундное разрешение — сумма не
  вырождается в ноль, как было бы на `steady_ms`); при `--parallel 5` и `--parallel 8`
  e2e зелёный (8/8). Попутно подтвердилось, что заявленный в README сценарий «`--parallel`
  больше `POOL_MAX`» на больших N законно получает 503 — почему пул неблокирующий и как
  это видно по `exhausted_total`, уточнено в README.
- `bash scripts/run-shellcheck.sh --all` — rc=0.
- `./scripts/run-cppcheck.sh` — rc=0 (по изменённым файлам).
- `./scripts/run-clang-tidy.sh --all` — rc=0, 47 файлов, «no errors or warnings in project
  files» (первый прогон нашёл три `misc-include-cleaner` в новом коде — `<prometheus/counter.h>`
  и `<prometheus/histogram.h>` в `db_query_handler.*`, `<memory>` в `test_db_live.cpp` —
  добавлены).
- `./scripts/run-coverage.sh` — rc=0, production 6597/7210 = **91.50%** (было 6553/7164),
  полный scope 15575/16506 = 94.36%, ветви 30342/73759 = 41.14%, production-ветви
  7052/13212 = 53.38%, пер-файловых регрессий нет. Первый прогон поймал настоящую регрессию
  `db_query_executor.hpp 100% -> 60%` (пустая реализация по умолчанию в заголовке — непокрытая
  строка, см. «Чему учит»); после перевода метода в чисто виртуальные гейт зелёный без
  переанкора baseline. Провал шага юнит-тестов покрытия (`exit code 42`) во втором прогоне не
  воспроизвёлся на повторном запуске (2837/592 и 945/125, `all passed`) — известная флейкость
  живых NATS/Postgres-тестов в контейнере, а не регрессия раунда.

## Чему учит

- Планировочный документ — не догма: метрику, которая по конструкции всегда 0, надо не
  реализовывать, а объяснить, почему её нет (иначе через полгода её будут «чинить»).
- Предсоздание лейбл-серий — теперь повторяющийся приём в этом коде (round 155, round 157):
  любое новое семейство с метками надо материализовать в момент регистрации/проводки, иначе
  golden check не найдёт его в здоровом стеке.
- Пустая реализация по умолчанию, написанная прямо в интерфейсном заголовке (`) {}`), —
  это **непокрытая строка**: пер-файловый гейт сразу показал `db_query_executor.hpp
  100% -> 60%`, потому что тело нигде не исполняется (все наследники переопределяют метод).
  Чистый способ — сделать метод чисто виртуальным, как соседний `set_pool_metrics()`: тогда
  заголовок не растёт, а единственный наследник вне `DbExecutorBase` (тестовый fake в
  `test_coverage_ext.cpp`) получает пустой override в тестовом файле, который из гейта исключён.
  Если бы реализацию «по умолчанию» положили в `.cpp`, она всё равно осталась бы непокрытой.

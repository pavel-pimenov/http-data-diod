# round 215 — l2-db-spider: опрос баз по расписанию

## Задача

Добавить новый режим `l2-db-spider` в тот же бинарник `l2-proxy`: job-сервис,
который периодически (по таймеру) опрашивает заданные базы (Oracle/PostgreSQL)
произвольными read-only `SELECT`-запросами из INI-конфига и пишет результат в
JSON-файл на диске — по одному файлу `<db>_<query>.json` на каждый запрос.

Решение принято **вместо** NATS-клиентских запросов к БД из-за риска DoS-атаки:
spider не принимает клиентских запросов, работает по собственному расписанию.
Клиенты читают готовые JSON-файлы.

## Что сделано

### Конфиг
- `Config::Spider` (databases + queries) в `src/config.hpp`; добавлен `m_config_file`.
- `src/ini_parser.hpp` — минимальный INI-парсер (без зависимостей): секции `[name]`,
  ключи `key=value`, комментарии `;`/`#`, case-insensitive-поиск.
- `load_spider_config()` в `src/config.cpp` читает INI-файл `DB_SPIDER_CONFIG_FILE`
  (default `/spider/spider-config.ini`): секции `[spider]` (глобальные настройки),
  `[db:<имя>]` (коннект), `[query:<имя>]` (запрос, привязка к базе через `db=`);
  порядок баз сохраняется (порядок появления секций в файле).
- Имя запроса — из ключа `name` (если есть) или из имени секции; имена секций
  могут быть уникальными (префикс `<база>_<имя>`), чтобы избежать дубликатов.
- Секреты: пароль не хранится в INI — `expand_env_placeholders()` подставляет
  `%VAR%` из env; пароль баз задаётся в `.env` как `DB_SPIDER_PASSWORD`
  (placeholder `%DB_SPIDER_PASSWORD%` в spider-config.ini, в docker-compose
  прокидывается в контейнер).
- `validate_spider()` — валидация конфига; проверяет `m_config_file` и наличие баз.
- Режим `l2-db-spider` разрешён в валидации мода; исключён из L2 URL-валидации.
- Полный переход на INI: `DB_SPIDER_DATABASES` (JSON) удалён из кода и docker-compose.

### Метрики
- `SpiderMetrics`/`SpiderContext` в `src/app_context.hpp`, `init_spider_metrics()` в
  `src/app_context.cpp`; новый `m_spider_registry`, `m_spider_stats_history`.
- Восстановлены случайно удалённые `WorkerContext`/`ServerContext`.

### Движок
- `src/db_spider.hpp`/`src/db_spider.cpp` — `DbSpider` с `run()`, `run_poll()`,
  `poll_database()`, `execute_query()` (через `DbQueryHandler`), `write_output()`,
  `record_metrics()`.
- `DbQueryHandler::all_ready()` в `db_query_handler.hpp` — ожидание готовности
  пулов (Oracle init асинхронный), чтобы не писать 503 до инициализации.
- Выгрузка: **по одному файлу `<db>_<query>.json` на каждый запрос** (вместо
  одного `<db>.json` на базу) с метаданными `{generated_at, db, query, status,
  result}`; `result` содержит только данные запроса (columns+rows) или объект
  ошибки {code, message}; имя файла санитизируется.

### Main / порты
- `run_db_spider()` в `src/main.cpp`: metrics exposer на `19094`, health-сервер на
  `19095`, jthread c `DbSpider`; ветка `l2-db-spider` в `main()`.
- `kSpiderMetrics=19094`, `kSpiderHealth=19095` в `src/constants.hpp`.

### Сборка и тесты
- `src/CMakeLists.txt`: `db_spider.cpp` в `l2-proxy` и `test_components` +
  proxy-nats unity-группу; `test_db_spider.cpp` в test_components.
- `src/test_db_spider.cpp` (парсинг JSON, пустой конфиг, отклонение валидацией).
- `src/test_app_context.cpp:266` — в ожидаемые истории добавлен `"spider"`.

### Docker / окружение
- Сервис `l2-db-spider` в `docker-compose.yml` (порты 19094/19095, том
  `./spider-output:/spider`, healthcheck на 19095, `DB_SPIDER_ENABLED=true`,
  `./spider-config.ini:/spider/spider-config.ini:ro`).
- `spider-config.ini` — пример конфига Oracle ccm3t: shared pool, таблспейсы,
  таблицы с размерами.
- `rebuild-and-run.sh` — cleanup + `mkdir -p spider-output` + `touch spider-config.ini`.
- `src/docker-entrypoint.sh` — chown `/spider`.
- `.env.example` — документация spider-переменных (`DB_SPIDER_CONFIG_FILE` вместо
  `DB_SPIDER_DATABASES`).

### Наблюдаемость (дашборды/алерты/каталог)
- `scripts/metrics-golden-check.py` — 5 spider-метрик в `CONDITIONAL`.
- README — раздел про `l2-db-spider` (архитектура, INI-формат) + таблица метрик `l2_spider_*`.
- `scripts/generate-grafana-dashboards.py` — новый `create_spider_dashboard()`
  (uid `l2-db-spider`) + регистрация в `dashboard_definitions`.
- `scripts/generate-perses-dashboards.py` — spider-дашборд в `_DASHBOARD_BUILDERS`.
- `alerts/l2-alerts.yml` — алерт `L2DbSpiderUnhealthy` (`min(l2_spider_healthy) < 1`).
- `scripts/vmagent.yml` — добавлен scrape-target `l2-db-spider:19094` (без него
  spider-метрики не попадали в VM, golden-check ругался); неиспользуемый
  дубликат `vmagent/vmagent.yml` удалён (монтируется именно `scripts/vmagent.yml`).

## Метрики

| Метрика | Тип | Метки | Назначение |
|---|---|---|---|
| `l2_spider_polls_total` | counter | `outcome` | Циклы опроса баз (ok/error) |
| `l2_spider_queries_total` | counter | `db`, `query`, `status` | Выполненные запросы |
| `l2_spider_last_poll_duration_seconds` | gauge | — | Длительность последнего цикла |
| `l2_spider_last_success_timestamp` | gauge | — | Unix-время последнего успешного JSON |
| `l2_spider_healthy` | gauge | — | Здоровье spider (0/1) |

## Проверка

- `test_components` — все прошли (102 с, 3091 assertions / 632 test cases);
  `test_proxy_core` — 952 assertions / 126 test cases.
- `rebuild-and-run.sh` — сборка зелёная, `l2-db-spider` Up/healthy.
- Live-прогон на 3 базах (ccm2/ccm4/ccm5, пользователь l2common): все 9 запросов
  успешны — shared_pool (500/364/500 строк), tablespaces (40/32/36), table_sizes
  (135/266/142). Запрос таблспейсов сначала давал ORA-00942 (user_tablespaces/
  user_data_files недоступны) и ORA-00904 (v$tablespace не имеет contents/status) —
  исправлен на `v$tablespace`+`v$datafile` без служебных колонок.
- Spider-метрики scrape в VM через `scripts/vmagent.yml` (`l2-db-spider:19094`,
  метка `vm`); golden-check 102/102.
- `message_counter.py --iterations 1 --concurrent 1 --dup-check` — Success.
- `ci-gate.sh offline` — rc=0: dashboards 10/10 (вкл. l2-db-spider 5 panels),
  metric consistency 122/122 (C++ == dashboards == Perses == README == golden),
  alert rules 36, env 105/105, docker-context OK.

### Полный фетч (без обрезания) + компактный JSON
- `DbRowCollector::try_add()` (`db_query_utils.hpp`): `max_rows <= 0` = без лимита
  (полный фетч, никогда не truncated).
- `db_query_handler.cpp`: `max_rows == -1` передаётся executor'у как есть (безлимит),
  вместо дефолта.
- Валидация spider разрешает `max_rows = -1`; в `spider-config.ini` все запросы
  `max_rows = -1`.
- Компактный JSON: `data.dump(-1, ' ', false, json::error_handler_t::replace)` —
  без отступов (меньше файл) + `error_handler_t::replace` заменяет невалидные
  UTF-8 байты (Oracle `sql_text`) на U+FFFD вместо падения `type_error.316`.

### Убраны flat_set/flat_map (уменьшение исходников)
- `header_utils.hpp`: `std::flat_set` → `std::unordered_set<std::string>`, убраны
  `#include <set>`/`<flat_set>`.
- `db_query_handler.hpp`: `std::flat_map` → `std::map`, убран `<flat_map>`, убраны
  `#if __has_include` ветки.

### Shared pool — полный SQL из sys.v_$sqlarea
- Запрос shared_pool: полный список колонок `v$sqlarea` (без LOB `sql_fulltext`;
  `sql_text` — VARCHAR2), `ORDER BY buffer_gets DESC`, `program_line#` (не `program_line`).
- `L2_DB_SPIDER_MEM_LIMIT` 512m → 2048m: полный фетч v$sqlarea на ccm4 даёт 113MB
  JSON, не влезал в 512MB → контейнер перезапускался (OOM-pressure).
- Live: ccm2 10315 / ccm4 67144 / ccm5 17932 строк, 81 колонка, статус ok,
  контейнер стабилен (restarts=0, healthy=1).

## Примечания
- Oracle-таргет `runtime-db` (как у воркера) — OCI-клиент только в этом образе.
- `.env` (не в git): `L2_WORKER_DOCKER_TARGET=runtime-db`,
  `DB_SPIDER_PASSWORD=common` (пароль баз, подставляется в INI).
- Конфиг spider — INI-файл `spider-config.ini` (монтируется read-only); полный
  переход с `DB_SPIDER_DATABASES` (JSON) на INI.
- Пароли в INI не хранятся — placeholder `%DB_SPIDER_PASSWORD%` заменяется из env.

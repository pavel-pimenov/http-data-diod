# Round 66: Perses — лёгкая альтернатива Grafana: контейнер в стеке и нативный конвертер дашбордов

## Мотивация

Для сравнения досок метрик хотелось лёгкую альтернативу Grafana: контейнер без БД, без залогина,
с тем же набором панелей и одним каталогом метрик. Perses (проект из окружения CNCF/OpenTelemetry)
подходит: статичный Go-бинарник на distroless-образе, файловое JSON-хранилище, полный REST API,
DSL-дашборды (JSON/YAML).

Выбран путь в две фазы:
1. миграция графановских JSON через `POST /api/migrate` (данные о деградации см. ниже);
2. нативный конвертер — потому что migrate заменяет запросы с переменной `${vm:regex}`
   (есть во всех ручных досках) на заглушку `migration_from_grafana_not_supported`.

## Изменения

### 1. `docker-compose.yml` — сервис `perses`

- Образ `persesdev/perses:v0.54.0` (последний стабильный на 2026-07-29, amd64+arm64 на Docker Hub).
- `ports: "8089:8080"` — хост-порт 8080 занят (ssh-слушатель + посторонний контейнер
  `hp-thinpro-update`), поэтому наружу проброс на 8089.
- Named volume `perses-data:/perses` — файловая БД (JSON-документы), прописана в `volumes:`.
- `depends_on: l2-proxy`, сеть `l2_network`, `restart: unless-stopped`.
- Docker-healthcheck сознательно не добавляем: базовый образ distroless (нет curl/sh, только
  `/bin/perses`); готовность проверяется host-curl в rebuild-and-run.sh. Консистентно с Grafana
  (health-check.sh не трогаем).

### 2. `rebuild-and-run.sh`

- `docker rm -f perses` добавлен в блок очистки конфликтующих контейнеров.
- После grafana-блока — синк Perses: ожидание `GET /api/v1/projects` (до 15 попыток), затем
  `PERSES_URL="$PERSES_URL" python3 scripts/generate-perses-dashboards.py`. Если Perses недоступен —
  подсказка как синкать вручную.

### 3. Новый `scripts/generate-perses-dashboards.py` (chmod +x)

- Определения дашбордов берёт из `scripts/generate-grafana-dashboards.py` (единый источник):
  модуль грузится через `importlib.util.spec_from_file_location` (в имени файла дефисы — обычный
  import невозможен). Список `DASHBOARDS` — 8 пар (функция генератора, uid).
- `PersesAPI`: retries, проверка соединения, `create_project` (`l2`), `create_global_datasource`
  (`prometheus`, `default: true`, plugin `PrometheusDatasource` + `HTTPProxy` →
  `http://victoria-metrics:8428`), `migrate`, `dashboard_exists`, `save_dashboard` (PUT/POST).
- Режимы CLI: `--mode native` (default) | `migrate`; `--check` — offline-валидация конвертера;
  `--dry-run`, `--output-dir`, `--datasource-name/--datasource-url`, `--timeout/--retries`.
- Нативный конвертер (`convert_dashboard_to_perses`):
  - панели: `timeseries`/`graph` → `TimeSeriesChart`, `stat` → `StatChart`, `bargauge` → `BarChart`.
    Плагина `BargaugeChart` в v0.54.0 нет (список плагинов получен из живого `GET /api/v1/plugins`);
    сам `/api/migrate` для bar-gauges тоже отдаёт `BarChart`.
  - запросы → `PrometheusTimeSeriesQuery`; `_rewrite_promql` заменяет `${vm:regex}`/`${vm}` → `$vm`;
  - unit-маппинг: `short→decimal`, `percentunit→percent-decimal`, `ns→nanoseconds` и т.д.;
    неподдерживаемые units → пустая строка;
  - переменные из grafana `templating` (`label_values(metric, label)`) → `ListVariable`
    + `PrometheusLabelValuesVariable` (например `vm`: `labelName: vm`, `matchers: ["up"]`,
    `sort: alphabetical-asc`) — shape сверен с результатом миграции;
  - layouts: `kind: Grid`, `x/y/w/h` переносятся 1:1 (сетка Perses — 24 единицы, как Grafana),
    абсолютный `y` сохраняется; `$ref` панелей вида `{индекс_раскладки}_{n}`;
  - `duration: 1h`, `refresh: 1m`, берутся из генератора дашбордов;
  - `_validate_perses_dashboard`: refs резолвятся, нет пустых запросов, нет строк
    `migration_from_grafana_not_supported`, нет незаменимых `${...}`.
- Лишние сущности не создаются: проект/датасорс создаются один раз («already exists, skip»),
  дашборды перезаписываются идемпотентно через PUT.

## Миграция vs нативный (результаты сравнения)

Замер деградации при `--mode migrate` (панелей всего / деградировано, причина — `${vm:regex}`):

| Дашборд | всего / деградировано |
| --- | --- |
| l2-distributed-tracing | 8 / 14 |
| l2-sentry-delivery | 3 / 4 |
| l2-proxy | 45 / 69 |
| l2-worker | 24 / 32 |
| l2-server | 9 / 12 |
| l2-slo-tracking | 13 / 24 |
| nats-dashboard | 16 / 24 |
| nginx-metrics | 4 / 0 (без переменных — мигрирует чисто) |

Нативный конвертер даёт те же панели без заглушек: запрос вида
`l2_tracing_queue_size{vm=~"$vm"}` + ListVariable `vm`, раскладки с заголовками
(dashboard l2-distributed-tracing: «Обзор трассировки», «Задержки трассировки»,
«Sentry/GlitchTip performance»).

## Проверка

- `python3 scripts/lint-python.py scripts/generate-perses-dashboards.py` — 0 issues (NOEOL доснят).
- `python3 scripts/generate-perses-dashboards.py --check` — offline-валидация 8/8.
- `./rebuild-and-run.sh` — сборка зелёная, `perses` up на 8089; синк нативного режима — 8/8
  (PUT по всем дашбордам).
- Прокси датасорса живёт: `GET /proxy/globaldatasources/prometheus/api/v1/query?query=up` — серии с
  label `vm="MacBook-67CF"`; `query_range` для `rate(l2_proxy_client_requests_total[5m])` — данные.
- `./health-check.sh all` — rc=0; `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- `./scripts/ci-gate.sh all` — unit, offline, runtime (message_counter+dup-check, DB gateway e2e 7/7,
  golden 80/80, consistency offline+runtime 84/84/84/84) — все шаги «passed».

## Побочные эффекты / замечания

- Хост-порт 8080 не занят дополнительно: Perses висит на 8089, WEB-UI по
  `http://localhost:8089`, REST API считан оттуда же.
- Хранилище дашбордов — named volume `perses-data`; compose down не теряет доки, при `up` они
  перезаписываются PUT'ом из скрипта.
- health-check.sh и метрические гейты (metrics-consistency / golden) не менялись: Perses-доски
  порождаются из того же каталога метрик генератора, консистентность имён метрик не затрагивается.
- Доступность `--check` позволяет гонять конвертер офлайн (unit-style) без поднятого стека.
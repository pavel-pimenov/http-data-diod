# round 213 — конфигурируемые зеркала REGISTRY/APT_MIRROR per-contour, NATS 2.15.2 в Dockerfile

## Задача

Сделать зеркала закрытых контуров конфигурируемыми: в разных закрытых
контурах используются разные docker registry и apt-зеркала. Раньше дефолты
(`docker-registry.dp.nlmk.com/`, `repos.dp.nlmk.com/...`) были жёстко зашиты в
docker-compose.yml, rebuild-and-run.sh и Dockerfile, что мешало переиспользованию
в других контурах (напр. `repo.ia.nlmk/k8s-oci/dockerhub/` + `repo.ia.nlmk/artifactory/ubuntu_repo/`).

## Конфигурируемые зеркала

Сделано: дефолты зеркал вынесены в `.env` (per-contour), в коде — пустой
fallback (Docker Hub / distro-дефолт).

### docker-compose.yml

Все `image: ${REGISTRY:-docker-registry.dp.nlmk.com/}...` заменены на
`image: ${REGISTRY:-}...` — пустой fallback = Docker Hub. Каждый контур задаёт
`REGISTRY` в своём `.env`.

Build args:
- `REGISTRY_MIRROR: "${REGISTRY:-}"` — префикс registry для внешних образов;
- `APT_MIRROR: "${APT_MIRROR:-}"` — apt-зеркало (пусто = distro-дефолт);
- `BASE_IMAGE: "${BASE_IMAGE:-${REGISTRY:-}ubuntu:26.04}"`.

### rebuild-and-run.sh

`export REGISTRY="${REGISTRY:-}"`, `export APT_MIRROR="${APT_MIRROR:-}"` — берутся
из .env (загружается в начале). Комментарий обновлён: per-contour, примеры.

### src/Dockerfile

ARG дефолты убраны:
- `ARG REGISTRY_MIRROR=` (было `=docker-registry.dp.nlmk.com/`);
- `ARG APT_MIRROR=` (было `=https://repos.dp.nlmk.com/...`).

Также обновлён `FROM ${REGISTRY_MIRROR}nats:2.14-alpine` → `2.15.2-alpine`
(синхронизация с docker-compose, NATS обновлён в round-212).

### .env (текущий контур, в gitignore)

Добавлены зеркала текущего контура dp.nlmk:
```
REGISTRY=docker-registry.dp.nlmk.com/
APT_MIRROR=https://repos.dp.nlmk.com/artifactory/archive-ubuntu-remote/
```

### .env.example

Документированы зеркала per-contour с примером другого контура:
```
# REGISTRY=repo.ia.nlmk/k8s-oci/dockerhub/
# APT_MIRROR=http://repo.ia.nlmk/artifactory/ubuntu_repo/
```
(адреса контуров могут отличаться).

## Фикс сборки lint-контейнеров

`run-shellcheck.sh`, `run-clang-tidy.sh`, `run-cppcheck.sh` собирают builder-образ
(`docker build --target builder`) без build args. После убирания дефолтов из
Dockerfile apt не находил пакеты (curl: `Unable to locate package`). Добавлено:
- загрузка `.env` (set -a / set +a);
- передача `--build-arg REGISTRY_MIRROR=...`, `APT_MIRROR=...`, `BASE_IMAGE=...`
  в `docker build --target builder` в `ensure_builder_image()`.

## Вынос общих хелперов sh-скриптов в `scripts/lib-common.sh`

Дублированный код раскидан по lint-скриптам и гейтам — вынесен в общий
source-файл `scripts/lib-common.sh` (вместе с `scripts/contour-build-args.sh` для
build args). Что вынесено:
- `log_info`/`log_warn`/`log_error` + цветовые переменные (`RED`/`GREEN`/`YELLOW`/`NC`) —
  повторялись в 8 скриптах (`run-shellcheck.sh`, `run-clang-tidy.sh`, `run-cppcheck.sh`,
  `ci-gate.sh`, `pre-commit.sh`, `install-health-cron.sh`, `health-check.sh`);
- инициализация путей `SCRIPT_DIR`/`PROJECT_ROOT`/`cd`;
- `ensure_builder_image()` — сборка builder-образа (с contour-зеркалами), принимает
  каталог-контекст аргументом;
- `ensure_version_header()` — генерация `src/l2-proxy-version.h` на свежем чекауте.

`run-shellcheck.sh`/`run-clang-tidy.sh`/`run-cppcheck.sh` подключили `lib-common.sh`
вместо прямого `contour-build-args.sh` и удалили локальные копии `ensure_*`/логгеров.
Итог: −146 строк дублей, +1 хелпер. Проверено: `ci-gate.sh unit`, `health-check.sh`,
`install-health-cron.sh --status`, `run-shellcheck.sh --all` (31 файл, rc=0),
pre-commit зелёный.

## Вынос общих Python-хелперов в `scripts/metrics_common.py`

`metrics-consistency-check.py` и `alerts-consistency-check.py` дублировали
`load_module`, константы (`METRIC_LITERAL`, `CREATE_CALL`, `HISTOGRAM_SUFFIXES`,
`LAZY_PREFIXES`, `SERVICE_ENDPOINTS`, `SHARED_PREFIXES`) и парсеры
(`normalize`, `cpp_registered_metrics`, `parse_exposition`, `scrape_metrics`,
`compare`, `split_lazy`). Вынесено в новый `scripts/metrics_common.py`.

`metrics-consistency-check.py` импортирует из него и реэкспортирует
(`METRIC_LITERAL`, `cpp_registered_metrics`, ...) — скрипты, которые загружают
его через `load_module` (напр. `alerts-consistency-check.py`), работают без
изменений. `alerts-consistency-check.py` тоже переведён на `metrics_common`.
`sys.path` на `scripts/` добавлен в обоих, чтобы `import metrics_common`
работал и при загрузке через importlib (тесты `test_metrics_consistency.py`).

Итог: −96 строк дублей, +1 модуль. Проверено: 95 Python unit-тестов, оба
скрипта `--offline` (117 метрик / 35 правил согласованы), `ci-gate.sh offline`,
pre-commit зелёный.

## Единый async-стиль e2e-тестов + `scripts/e2e_utils.py`

E2E-скрипты раньше смешивали sync (`urllib`/`subprocess`/`ThreadPoolExecutor`)
и async (`aiohttp`/`nats-py`) стили с копи-пастом хелперов. Переписаны на
единый async-стиль с общим модулем `scripts/e2e_utils.py`:

- `log` — лог с timestamp;
- `docker` — async-обёртка docker CLI (captures stdout/stderr);
- `nats_url` — авторизованный NATS URL (E2E_NATS_URL / E2E_NATS_TOKEN /
  NATS_TOKEN из .env);
- `http_get`/`http_post` — async aiohttp-хелперы (JSON in/out, сети-ошибки → (-1, ""));
- `wait_for`/`wait_http` — поллинг async-предиката.

Переведены на `e2e_utils`:
- `e2e-worker-reconnect-test.py` (log/docker/nats_url/wait_for);
- `e2e-graceful-shutdown-test.py` (docker stop/inspect/logs/start + health через
  aiohttp вместо curl);
- `db-gateway-e2e-test.py` (urllib+ThreadPool → aiohttp+asyncio.gather,
  HTTPError-обработка через проверку status);
- `sentry-e2e-test.py` (urllib/subprocess → aiohttp/e2e_utils, HTTP-пост async).

`glitchtip-performance-e2e.py` оставлен на subprocess (psql через docker exec —
не HTTP, async не даст выгоды). Все скрипты совместимы с Python 3.7
(`list[str]` → `List[str]`). Проверено: `lint-python` 36 файлов без замечаний,
pre-commit зелёный.

## NATS 2.15.2 в Dockerfile

`FROM nats:2.14-alpine` → `FROM nats:2.15.2-alpine` (был не синхронизирован с
docker-compose, обновлённым в round-212). Комментарий в Dockerfile обновлён.

## Проверки

- `docker compose config` с REGISTRY='' → Docker Hub (`nats`, без префикса);
- с `REGISTRY=repo.ia.nlmk/k8s-oci/dockerhub/` → `repo.ia.nlmk/k8s-oci/dockerhub/nats`;
- из `.env` (dp.nlmk) → `docker-registry.dp.nlmk.com/nats...`;
- сборка `./rebuild-and-run.sh` зелёная (health passed, дашборды синкнуты);
- NATS v2.15.2, Oracle ping ok, message_counter Success;
- pre-commit passed (builder-образ собран с правильными build args).

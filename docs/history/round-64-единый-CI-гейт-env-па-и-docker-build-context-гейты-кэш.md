# Round 64: единый CI-гейт, env-var и build-context гейты, кэш docker-слоёв в CI

## Мотивация

После устранения красного CI в round 63 оставались разрозненные гейты: pre-commit и GitHub Actions
дублировали друг друга (pep-одинаковые шаги могли разойтись), отсутствовали автоматические проверки
соответствия env-переменных C++ и `docker-compose.yml`, docker build-контекст не проверялся на
ссылки Dockerfile в исключённых `.dockerignore`-файлах, а сборка образов в каждом CI-прогоне шла
«с нуля» (docker слои не кэшировались между прогонами).

## Изменения

### 1. Единый гейт `scripts/ci-gate.sh` (point of truth для pre-commit и CI)

Новый скрипт с подкомандами:

- `unit` — Python unit-тесты (`python3 -m unittest discover -s tests`), без контейнеров;
- `offline` — metric-name consistency (`--offline`), env-var ↔ compose, docker build-context;
- `runtime` — message_counter (`--dup-check`), DB gateway e2e, golden metrics (`--traffic --db`),
  metric-name consistency (`--runtime`); «стек вниз» / «postgres вниз» → warn c rc=0;
- `all` — unit + offline + runtime.

`SKIP_METRICS_CHECK=1` пропускает metric-name гейты (offline+runtime).

`scripts/pre-commit.sh` переведён на вызовы ci-gate.sh (`run_unit_tests` → `ci-gate.sh unit`,
`run_metrics_check` → `ci-gate.sh offline` + `ci-gate.sh runtime`; отдельная `run_message_test`
удалена — message_counter теперь первый шаг runtime-гейта).

`.github/workflows/ci.yml`: шаги «unit tests», «offline» и «runtime» теперь вызывают `ci-gate.sh`
(offline выполняется до сборки образов — fast-fail; runtime — единый шаг после подъёма стека вместо
трёх независимых message_counter/db/e2e/golden/consistency).

### 2. Гейт env-переменных `scripts/env-consistency-check.py`

- Извлекает из исходников все `get_env_*` (config.cpp) и прямые `getenv` в main.cpp → 98 прод-переменных.
- Парсит `docker-compose.yml` (блоки `environment`/`args`, формы `- KEY=`, `KEY:`, `${KEY:...}`).
- Каждая C++-переменная обязана присутствовать в compose (`CRASH_DUMP_DIR` был единственным
  пропущенным — добавлен `- CRASH_DUMP_DIR=/crash-dumps` во все три сервиса l2-server/l2-proxy/l2-worker;
  значение совпадает с дефолтом `g_default_crash_dump_dir = "/crash-dumps"`). Итог: 98/98, 0 missing.
- Переменные compose, не читаемые C++ (48) — informational (rc=0): это runtime/scripts-only
  (APT_MIRROR, GF_*, POSTGRES_*, L2_*_MEM_LIMIT и т.п.).
- Юнит-тесты: `tests/test_env_consistency.py` (11 шт.).

### 3. Гейт docker build-контекста `scripts/docker-context-check.py`

- Матчер `.dockerignore` по Docker-семантике: bare-паттерн матчит любой сегмент пути, паттерн
  со `/` или завершающим `/` — root-relative, `!`-отмена, «последнее правило побеждает».
- Из Dockerfile извлекаются ссылки на локальные файлы (COPY/ADD без `--from=` и абсолютных путей;
  `./*.sh|py` в RUN/CMD — бинарники `./l2-proxy` не считаются).
- Проходит для текущего дерева: 324 файла tracked, 25 исключено, 3 refs (generate_version.sh,
  docker-entrypoint.sh, nats). Регрессионные тесты «битого» `.dockerignore` ловят поломку.
- Юнит-тесты: `tests/test_docker_context.py` (15 шт.).

### 4. Кэш docker-слоёв между CI-прогонами

- `rebuild-and-run.sh`: новая `build_images()` — при `L2_BAKE_CACHE=gha` (CI) сборка идёт через
  `docker buildx bake -f docker-compose.yml` с `--set '*.cache-from/cache-to=type=gha'` и явными
  тегами `<project>-l2-proxy|worker:latest` (bake сам не выводит compose-теги). Локальный путь
  `docker compose build` не меняется. Retry-ветка `--no-cache` сохранена.
- `.github/workflows/ci.yml`: шаг `docker/setup-buildx-action@v3` (docker-container драйвер) перед
  сборкой; шаг «Build images» получает `L2_BAKE_CACHE=gha`. Слои, построенные в прогоне, греют
  следующий.

### 5. trace_logger: покрытие реальной ветки «Jaeger мёртв, Sentry жив»

Эмпирический анализ round 63 показал: все «непокрытые» строки `trace_logger.cpp` — это
`Logger::*`-стейтменты (артефакт атрибуции gcov) либо извне недостижимые ветки (пустой batch,
null-клиент пула, `catch(...)`). Единственная реально новая комбинация путей — независимая
Sentry-доставка при мёртвом Jaeger-таргете: добавлен тест
«sentry delivers transactions when Jaeger target is dead» (healthy Sentry-сервер + мёртвый Jaeger:
`sentry_sent() >= 1`, `sentry_failed() == 0`, Jaeger-счётчик `failed() >= 1`). У структуры
`SentryTracingEnv` добавлен accessor `failed()`. Счёт C++ unit-тестов вырос на 1 (C++-тесты гоняются
в контейнере на этапе сборки Dockerfile).

## Проверка

- Python unit-тесты: 78 зелёных (45 → +8 golden → +11 env → +15 docker-context → +1 C++).
- `./scripts/ci-gate.sh offline` — metric consistency 84/84/84/84, env 98/98, docker-context 324/3 — rc=0.
- `./rebuild-and-run.sh` — сборка зелёная (C++ unit-тесты в build-этапе Dockerfile прошли).
- `./health-check.sh all` — все сервисы OK.
- `./scripts/ci-gate.sh runtime` — message_counter + dup-check, DB gateway e2e 7/7, golden 80/80,
  consistency offline+runtime — rc=0.
- `./scripts/pre-commit.sh` (рефакторенный) — health → unit → offline+runtime → clang-tidy, все шаги rc=0.
- CI-прогон после push — все 3 job success (Build+unit+NATS smoke, Coverage gate, clang-tidy sweep).

## Побочные эффекты

- `docker-compose.yml`: `CRASH_DUMP_DIR=/crash-dumps` в l2-server/l2-proxy/l2-worker.
- macOS-квайч pre-commit: `nproc: command not found` в run-clang-tidy.sh — существовал до раунда,
  clang-tidy всё равно отрабатывает (jobs пуст).
# Round 224 — Починка CI: сборка только runtime-таргета

## Задача
CI (kaniko в `.gitlab-ci.yml`) падал на сборке всего Dockerfile:
`failed to get fileinfo for .../src/scripts/spider_report_pdf.py: no such file or directory`
(rc=1). Дополнительно в логе был шум clang-tidy-ошибок consteval из spdlog/fmt.

## Причина
Dockerfile в `src/Dockerfile` использует **два разных build-контекста**:
- runtime-таргеты (`builder`, `runtime-base`, `runtime*`) — src-относительные пути
  (`COPY nats ./nats`, `COPY . .`, `COPY docker-entrypoint.sh`);
- AI/heat/pdf-стадии (`pdf-report`, `spider-ai`, `heat-scraper`, `heat-ai`) — корневой
  контекст (`COPY scripts/spider_report_pdf.py` — скрипты лежат в корне репо `scripts/`).

docker-compose это разрешает двумя build-конфигами (`context: ./src` для runtime-сервисов,
`context: .` для AI-сервисов). CI же собирал **весь** Dockerfile одним контекстом
`$CI_PROJECT_DIR/src` без `--target`, из-за чего kaniko доходил до стадии `pdf-report`
и падал на отсутствующем `src/scripts/spider_report_pdf.py`. Плюс по умолчанию (без
`--target`) CI собирал последнюю стадию `runtime-valgrind` (с valgrind) — не прод-образ.

## Реализация
### `.gitlab-ci.yml`
Добавлен явный таргет сборки `--target runtime`:
```yaml
--context $CI_PROJECT_DIR/src
--dockerfile $CI_PROJECT_DIR/src/Dockerfile
--target runtime
```
- контекст остаётся `src` (совпадает с `l2-proxy`/`l2-server` в docker-compose);
- `runtime` — тот же прод-таргет, что и `l2-proxy` (`L2_PROXY_DOCKER_TARGET:-runtime`)
  и `l2-server` (потребляет CI-образ `http-data-diod-l2-proxy:latest`);
- kaniko строит только цепочку зависимостей `runtime`: `ubuntu-base`, `builder`,
  `runtime-base`, `runtime` — **AI/heat/pdf/coverage/lint-стадии не собираются**,
  значит и fileinfo-ошибка `src/scripts/...`, и clang-tidy-шум из стадии `lint`
  (там `|| true`, не фейлит) исчезают.

## Проверка
- `docker buildx build -f Dockerfile --target runtime <src-контекст>` — образ собран
  (sha256:c10c..., naming ci-verify-runtime:test), прошли стадии ubuntu-base → builder →
  runtime-base → runtime; `COPY docker-entrypoint.sh` резолвится из src.
- `--no-cache` прогон подтвердил: собираются только `nats-server-image`, `ubuntu-base`,
  `builder`, `runtime-base` (pdf-report/spider-ai/heat-ai/heat-scraper/lint/coverage
  отсутствуют в выводе).

## Ключевое решение
CI-образ потребляется сервисом `l2-server` (прод `runtime`), поэтому CI не должен
собирать AI/heat/pdf-стадии вообще — они собираются отдельно docker-compose с корневым
контекстом. `--target runtime` убирает и ошибку контекста, и clang-tidy-шум.

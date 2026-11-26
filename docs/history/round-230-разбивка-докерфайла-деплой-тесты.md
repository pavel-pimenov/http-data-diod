# Round 230 — Разбивка Dockerfile на деплой и тесты (Dockerfile + Dockerfile.test)

## Задача
Kaniko (CI) не поддерживает BuildKit `RUN --mount=type=cache`. Раньше их убрали
из единого `src/Dockerfile` (round 229), но это лишило локальные сборки кеша.
Чище — разбить: один Dockerfile для деплоя (kaniko, без `--mount`, без тестов),
второй — для тестов (docker buildx, с `--mount`).

## Реализация
### `src/Dockerfile` — только деплой
Из единого файла удалены стадии `test`, `coverage`, `lint` (они переехали в
`Dockerfile.test`). Осталось:
- ubuntu-base, nats-server-image, builder (компиляция l2-proxy, без тестов);
- pdf-report, spider-ai, heat-scraper, heat-ai (для docker-compose);
- runtime-base, runtime, runtime-asan, runtime-db, runtime-profiler,
  runtime-valgrind, oracle-libs.
- Без `--mount` (kaniko-совместимо), без coverage/lint.

### `src/Dockerfile.test` — новый, для тестов
Собран из старых стадий (с восстановленными `--mount`):
- ubuntu-base, nats-server-image, builder (с `--mount` apt/ccache/build-dir);
- **test** (юнит-тесты test_components/test_proxy_core);
- **coverage** (сборка --coverage + gcovr);
- **lint** (clang-tidy).
- Предназначен для локального docker buildx / `docker compose run` на отдельной
  машине, НЕ для kaniko/CI.

### `scripts/run-coverage.sh`
`-f src/Dockerfile` → `-f src/Dockerfile.test` (coverage-стадия переехала).
Комментарий про гейт обновлён.

### `scripts/coverage-regression-check.py`
Комментарий: `src/Dockerfile` → `src/Dockerfile.test`.

## Проверка
- `src/Dockerfile --target runtime` — exit 0 (kaniko-совместим, без --mount).
- `src/Dockerfile.test --target test` — юнит-тесты прошли (test_components
  3091/632, test_proxy_core 952/126).
- `src/Dockerfile.test --target coverage` — coverage отчёт отрисован, exit 0.
- docker-compose: все сервисы собраны — l2-proxy (runtime), l2-worker (runtime-db),
  l2-spider-ai, l2-heat-ai, l2-heat-scraper (exit 0).
- `./rebuild-and-run.sh` — полная сборка, все контейнеры healthy;
  message_counter.py --iterations 1 --concurrent 1 — exit 0, no message loss.

## Замечание
`rebuild-and-run.sh` падает на `source .env` из-за JSON array `L2_SERVER_URLS`
(строка 21) — существующий баг, не связанный с разбивкой; для проверки строку
временно закомментировали, после вернули.

## Ключевое решение
Два независимых Dockerfile: `src/Dockerfile` (деплой, kaniko-friendly, без
`--mount`) и `src/Dockerfile.test` (тесты, docker buildx, с `--mount`).
CI собирает только деплой (runtime) через kaniko; тесты/покрытие/линт гоняются
локально через Dockerfile.test.

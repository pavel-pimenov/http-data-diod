# round 267 — два профиля сборки Docker-образов

## Проблема

После слияния архивной ветки образы собирались только при ручной подстановке
`PIP_INDEX_URL=https://pypi.org/simple`: пип-стейджи в `src/Dockerfile`
дефолтили на внутренний artifactory, а `.env.example` содержал активные
адреса зеркал dp.nlmk. То есть «сборка на компе с интернетом» требовала
знать про внутренний индекс и обходить его, а закрытый контур не имел
явного включателя.

## Решение

Введён единый переключатель `BUILD_PROFILE` с двумя значениями:

- `public` (по умолчанию) — Docker Hub, стандартные apt-источники Ubuntu,
  `https://pypi.org/simple`. `.env` не нужен; если в старом `.env` остались
  адреса зеркал, публичный профиль их игнорирует.
- `contour` — закрытый контур. `REGISTRY` (префикс внешних образов),
  `APT_MIRROR`, `PIP_INDEX_URL` и необязательный `BASE_IMAGE` берутся из
  `.env`; дефолты dp.nlmk в `scripts/build-profile.sh` — только документированный
  пример, другой контур переопределяет их своими адресами.

`resolve_build_profile()` в новом `scripts/build-profile.sh` — общий резолвер
для `rebuild-and-run.sh` и `scripts/contour-build-args.sh` (lint/coverage).
Последний больше не делает `source .env` (там могут быть JSON-массивы и
произвольный shell), а читает только простые строки `VAR=value`, и передаёт
все четыре build-arg's, включая `PIP_INDEX_URL`. Вывод helper'а теперь
напрямую заполняет `CONTOUR_BUILD_ARGS` (без `local -n`): macOS использует
`bash 3.2`, где nameref — синтаксическая ошибка. Обновлены все вызывающие
скрипты, включая `run-coverage.sh`, который раньше вообще не передавал
зеркала в сборку coverage-образа.

Публичные дефолты перенесены в сами Dockerfiles: `ARG PIP_INDEX_URL` теперь
`https://pypi.org/simple`, а `--trusted-host` выводится из URL индекса —
адрес внутреннего зеркала больше не зашит в образе. В `docker-compose.yml`
дефолт `PIP_INDEX_URL` тоже публичный (8 блоков `build.args`). Заголовки
`src/Dockerfile` и `src/Dockerfile.test` описывают оба профиля.

`.env.example` теперь содержит `BUILD_PROFILE=public` и закомментированные
примеры зеркал; README получил раздел «Сборка образов: два профиля» с таблицей
профилей и порядком переключения.

## Проверка

- `./rebuild-and-run.sh` **без** `PIP_INDEX_URL` и без `.env` — rc=0;
  в логе `Build profile: public`, registry `<Docker Hub>`, apt `<distro
  defaults>`, pip `https://pypi.org/simple`, base `ubuntu:26.04`.
- `./health-check.sh all`, `python3 message_counter.py --iterations 1
  --concurrent 1`, `./scripts/ci-gate.sh all` — rc=0. Для runtime-метрик
  spider локально поднят с тестовыми учётными данными (без OracleClient
  опрос БД не выполняется, метрика `l2_spider_queries_total` — lazy).
- `bash -n` на изменённых скриптах, `./scripts/run-shellcheck.sh`,
  `docker compose config -q`, офлайн-гейты env/metrics consistency — зелёные.
- Резолвер проверен для public, contour с дефолтами dp.nlmk и contour с
  кастомными зеркалами другого контура (`repo.ia.nlmk/...`) — значения
  build-arg'ов соответствуют профилю.
- C++-код не менялся; покрытие остаётся на уровне раунда 266 (production
  7495/8713 = 86.02%), `docs/coverage-baseline.json` не переписывался.

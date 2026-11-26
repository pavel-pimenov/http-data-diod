# Round 228 — Отключение стадии тестов в CI, только сборка runtime-образа

## Задача
Стадия тестов в CI не может пройти в раннере: юнит-тестам нужен запуск
`docker compose` (live NATS/PostgreSQL), которого в раннере нет. Deploy-сборка
была недоступна из-за этой стадии. Нужно полностью убрать стадию тестов и
оставить только сборку runtime-образа.

## Реализация
### `.gitlab-ci.yml`
- Удалён stage `test` и job `run_unit_tests` — стадия тестов полностью убрана
  из пайплайна.
- Остался один stage `deploy` и один job `build_docker` (kaniko `--target runtime`,
  пушит `$IMAGE_TAG` + `current`).
- Якорь `.kaniko_setup` и `--cache-repo` сохранены.
- `build_docker` переведён в **manual** на обе ветки (master и тег): сборка
  стартует **только вручную**, никогда автоматически после коммита/пуша.
- Добавлен `--skip-unused-stages=true`: kaniko по умолчанию строит **все** стадии
  Dockerfile (в т.ч. coverage и lint/clang-tidy), пока не достигнет `--target`.
  Флаг заставляет строить только стадии, нужные для `--target runtime`
  (ubuntu-base, builder, runtime-base, runtime) — coverage/lint/pdf/heat пропускаются.

### `src/Dockerfile` — без изменений
Стадия `FROM builder AS test` (раунд 226) оставлена в Dockerfile: она нужна для
локального запуска тестов на отдельной машине через `docker compose run` /
`--target test` — не в CI.

## Проверка
- YAML валиден: `stages: [deploy]`, единственный job — `build_docker`.
- Сборка runtime (`--target runtime`) не гоняет юнит-тесты (они в стадии `test`,
  не входящей в цепочку runtime).

## Ключевое решение
CI собирает и пушит только runtime-образ; юнит-тесты в CI отключены (требуют
docker compose / live NATS+PostgreSQL) и остаются только для локального запуска
на отдельной машине через Dockerfile-стадию `test`.

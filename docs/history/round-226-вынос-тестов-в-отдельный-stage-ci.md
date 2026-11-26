# Round 226 — Вынос юнит-тестов в отдельный stage CI-пайплайна

## Задача
Юнит-тесты (`test_components`, `test_proxy_core`) раньше выполнялись **внутри**
builder-стадии Dockerfile и гонялись при каждой сборке образа, в т.ч. в deploy-сборке
runtime. Нужно вынести их в отдельный stage пайплайна, чтобы:
- тесты стали отдельным гейтом (stage `test`);
- сборка runtime-образа не гоняла тесты повторно (быстрее).

## Реализация
### `src/Dockerfile` — новая стадия `test`
Тест-RUN-слои вынесены из `builder` в отдельную стадию `FROM builder AS test`:
- `builder` остаётся «чистым» (компиляция + копия `/app/out/l2-proxy`) — нужен
  `runtime-base` через `COPY --from=builder`;
- `test` — `FROM builder` + два RUN: `./test_components` и `./test_proxy_core`
  (последовательно, чтобы не OOM-kill тестовый Postgres на малых хостах);
- `runtime`-сборка теперь НЕ выполняет юнит-тесты (они не в цепочке builder→runtime);
- `coverage` (`FROM builder`) и его тесты покрытия не затронуты.

### `.gitlab-ci.yml` — stages `test` + `deploy`
- добавлен `stages: [test, deploy]`;
- общий kaniko-setup (image/auth/IMAGE_TAG) вынесен в якорь `&kaniko_setup`;
- **`run_unit_tests`** (stage `test`): kaniko `--target test` — собирает builder+test
  и останавливается, юнит-тесты служат гейтом; образ не пушится (нет destination);
  builder-слои переиспользуются из `--cache-repo`;
- **`build_docker`** (stage `deploy`): kaniko `--target runtime` — пушит
  `$IMAGE_TAG` + `current`; тесты не гоняются.

## Проверка
- `docker buildx build -f Dockerfile --target test` — собран, юнит-тесты прошли:
  `test_components` (3091 assertions / 632 cases), `test_proxy_core`
  (952 assertions / 126 cases), «All tests passed»;
- `docker buildx build -f Dockerfile --target runtime` — exit 0, в выводе НЕТ
  строк про unit-тесты (они вынесены из runtime-цепочки);
- `scripts/docker-context-check.py` — OK; YAML `.gitlab-ci.yml` валиден.

## Ключевое решение
Тесты вынесены в отдельную Dockerfile-стадию `test`, на которую CI-стадия `test`
собирает `--target test` (гейт), а `deploy` собирает `--target runtime` без тестов.
`--cache-repo` (раунд 225) позволяет переиспользовать builder-слои между job'ами.

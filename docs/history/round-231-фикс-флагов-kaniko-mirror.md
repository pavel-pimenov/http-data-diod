# Round 231 — Фикс флагов kaniko: пустой registry-mirror съедал --cache=true

## Задача
В CI-логе kaniko путал флаги: `Retrieving image ... from registry mirror --cache=true`
и `lookup --cache=true: no such host`. Это означало, что `--cache=true` не передавался
kaniko, т.е. **кеш слоёв не работал**, несмотря на заданные `--cache-repo`.

## Причина
В `.gitlab-ci.yml` стоял флаг `--registry-mirror $DOCKER_MIRROR`. Переменная
`DOCKER_MIRROR` в CI **не задана (пустая)**. Kaniko разбирает флаги по порядку:
`--registry-mirror` получил следующим аргументом `--cache=true` (а не значение mirror),
а сам `--cache=true` потерялся. Из-за этого:
- registry-mirror = `--cache=true` (неверно, DNS-lookup падал);
- `--cache=true` отсутствовал → кеширование не включалось.

## Реализация
### `.gitlab-ci.yml`
Убрана строка `--registry-mirror $DOCKER_MIRROR` (переменная пустая и не нужна —
kaniko и без неё работает, fallback на index.docker.io). Теперь `--cache=true`,
`--cache-repo`, `--cache-ttl` передаются корректно, и кеш слоёв включается.

## Проверка
- YAML валиден; `--cache=true` присутствует, `--registry-mirror` отсутствует.
- Сборка runtime идёт (лог: Resolved base name ubuntu-base/builder/runtime-base/
  runtime — только нужные стадии с --skip-unused-stages; coverage/lint не включены).
- Ожидается: в следующем CI-логе нет `registry mirror --cache=true` и `lookup
  --cache=true: no such host`; кеш-репо начинает работать.

## Ключевое решение
Пустой `--registry-mirror $DOCKER_MIRROR` съедал `--cache=true`; убран, чтобы флаги
кеширования передавались корректно.

## Дополнительно: облегчение деплойного runtime-образа
В `src/Dockerfile` (деплой) builder убран от тестовых/линтовых зависимостей и
Python:
- apt-install: убраны `clang-tidy`, `clang-format`, `cppcheck`, `shellcheck`,
  `catch2`, `postgresql` (сервер БД); остался `libpq-dev` (клиент для подключения
  к PostgreSQL, не сервер);
- `BUILD_TESTS=ON` → `BUILD_TESTS=OFF` во всех 3 ветках — тестовые бинарники не
  компилируются (тесты в `src/Dockerfile.test`);
- стадия `nats-server-image` и `COPY --from=nats-server-image` удалены — NATS-сервер
  нужен только live-тестам (в Dockerfile.test), не runtime.

Проверено: runtime-образ без python3/python, без test_components/test_proxy_core,
только `/root/l2-proxy` + docker-entrypoint.sh; docker-compose все сервисы собраны;
rebuild-and-run.sh exit 0; message_counter exit 0 (no message loss).

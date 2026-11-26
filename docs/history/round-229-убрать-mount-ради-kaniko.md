# Round 229 — Убрать BuildKit --mount из Dockerfile ради kaniko

## Задача
CI-сборка через kaniko с `--skip-unused-stages=true` падала на
`Dockerfile parse error line 48: Unknown flag: mount`. Kaniko не поддерживает
BuildKit `--mount=type=cache`.

## Причина
Dockerfile активно использовал BuildKit-монтирования `RUN --mount=type=cache`
(apt-кеш, ccache `/root/.ccache`, build-dir `/app/build`) — это фича BuildKit/docker
buildx, которую kaniko не понимает. Без `--skip-unused-stages` kaniko как-то
проходил мимо, но с флагом (нужным, чтобы не строить coverage/lint) парсер kaniko
становится строже и падает на `--mount`.

## Реализация
### `src/Dockerfile`
Все `RUN --mount=type=cache,...` заменены на обычный `RUN \`:
- apt-монтирования `/var/cache/apt`, `/var/lib/apt/lists`;
- ccache `/root/.ccache`;
- build-dir `/app/build` (id=appbuild-...);
- во всех стадиях: ubuntu-base, builder, test, coverage, pdf-report, runtime-base,
  runtime-asan, runtime-db, runtime-profiler, runtime-valgrind.

Компромисс: локальные сборки (docker buildx) теряют кеш apt/ccache/build-dir —
пересборка становится полной. Это осознанный выбор ради совместимости с kaniko.

### `.gitlab-ci.yml`
Без изменений — `--skip-unused-stages=true` остаётся, теперь kaniko сможет его
обработать (нет `--mount`).

## Проверка
- `docker buildx build -f Dockerfile --target runtime` — exit 0, сборка успешна
  (только предупреждения компиляции).
- `--mount` в Dockerfile отсутствует (grep = 0), `RUN \` без последующей пустой
  строки.
- Ожидается: kaniko в CI с `--skip-unused-stages=true` собирает только
  ubuntu-base/builder/runtime-base/runtime, без coverage/lint и без ошибки `--mount`.

## Ключевое решение
Убрать BuildKit-специфичные `--mount` из Dockerfile, чтобы kaniko мог собирать
только нужные runtime-стадии (`--skip-unused-stages`). Цена — потеря кеша в
локальных сборках.

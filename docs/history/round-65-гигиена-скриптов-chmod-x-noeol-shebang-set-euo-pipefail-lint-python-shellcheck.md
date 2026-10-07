# Round 65: гигиена скриптов — chmod +x, NOEOL, shebang, set -euo pipefail, lint-python и shellcheck в гейтах

## Мотивация

Скрипты в репозитории рассинхронизированы по исполняемым битам и общему «уходу»:

- конвенция репо «файл с shebang ⇒ executable» держалась не везде: 36 shebang-файлов были `+x`,
  19 — нет (`chmod 644`);
- `src/run-comprehensive-memory-analysis.sh` имел `+x`, но поломанный shebang — строка `ize#!/bin/bash`
  (первый символ missing) — запуск «напрямую» падал с `exec format error`;
- 8 py- и 10 sh-файлов без финального перевода строки;
- существующий эталонный py-чекер `scripts/lint-python.py` не был подключён ни к одному гейту
  (ни в ci-gate.sh, ни в pre-commit.sh, ни в CI) и накопил 245 замечаний (в осн. NOEOL и LONG100);
- три sh-скрипта без `set -e` (остальные 21/25 — с `set -euo pipefail`/`set -e`);
- смесь shebang'ов `#!/bin/bash` (21) и `#!/usr/bin/env bash` (2);
- отсутствовал shellcheck (shell-аналог clang-tidy) и защиты от регрессий CRLF/NOEOL
  (`.gitattributes`/`.editorconfig`).

## Изменения

### 1. Приведены к `+x` все 19 shebang-файлов без exec-бита

`dedup_test.py`, `dos2unix-recursive.py`, `fault_tolerance_test.py`, `load_test.py`,
`rate_limit_test.py`, `test-crash-handler.py`, корневые/`scripts/`-тесты, `src/docker-entrypoint.sh`,
`src/run_tests.sh`, `tests/test_*.py` (7 шт.) — все получили `100644 → 100755`.

### 2. Починен shebang `src/run-comprehensive-memory-analysis.sh`

Убран мусор перед `#!/bin/bash` (`ize#!/bin/bash` → `#!/bin/bash`) — скрипт стал запускаемым напрямую.

### 3. `scripts/lint-python.py` вычищен и подключён в offline-гейт

- Добавлена поддержка inline-игнора `# noqa: E501` для длинных строк (стандарт flake8/ruff).
- Вычищено всё накопленное: NOEOL в 8 py-файлах; 22 длинные строки переформатированы
  (message_counter.py — 17, metrics-golden-check.py — 2, dos2unix-recursive.py — 2,
  e2e-worker-reconnect-test.py — 1); 215 длинных строк данных в `scripts/generate-grafana-dashboards.py`
  (JSON-панели дашбордов) помечены `# noqa: E501` — переносить их нецелесообразно.
- Итог: `python3 scripts/lint-python.py` → 0 issues.
- Вызов добавлен первым шагом `g_offline()` в `scripts/ci-gate.sh` — теперь гейт держат и
  pre-commit, и CI (описание шага offline в ci.yml обновлено).

### 4. Финальный перевод строки добавлен в 10 `.sh`-файлов

`scripts/ci-gate.sh`, `scripts/install-git-hooks.sh`, `scripts/run-coverage.sh`,
`scripts/run-glitchtip-stack.sh`, `src/docker-entrypoint.sh`, `src/run-clang-format.sh`,
`src/run-comprehensive-memory-analysis.sh`, `src/run-docker-memory-analysis.sh`,
`src/run-with-heap-profiler.sh`, `src/run-with-valgrind.sh`.

### 5. Нормализован shebang

`#!/usr/bin/env bash` → `#!/bin/bash` в `scripts/run-clang-tidy.sh` и `scripts/run-coverage.sh`
(совпадает с большинством 21× `#!/bin/bash`).

Заодно устранён известный macOS-квайч run-clang-tidy.sh (зафиксирован в round-64 как «существовал
до раунда»): `mapfile` отсутствует в bash 3.2 на macOS и ронял clang-tidy-гейт до запуска — заменён
на while-read-цикл (как в новом run-shellcheck.sh).

### 6. `set -euo pipefail` добавлен в три скрипта

`src/generate_version.sh`, `src/run-with-heap-profiler.sh`, `src/run-with-valgrind.sh` — до этого
не имели `set -e` вообще (в отличие от остальных 21 sh-скриптов).

### 7. `.gitattributes` + `.editorconfig`

- `.gitattributes`: явные `text eol=lf` для sh/py/c/cpp/h/hpp/cc/inl/cmake/yml/json/md/txt/sql
  и др. (защита от CRLF-регрессий, важна и для exec-битов скриптов), бинарные артефакты
  (`*.so/*.a/*.o/*.gcno/*.gcda/*.prof/*.hprof`) — `binary`.
- `.editorconfig`: `utf-8`, `end_of_line=lf`, `insert_final_newline=true`,
  `trim_trailing_whitespace=true` (кроме `*.md`).

### 8. Shellcheck — пакет в builder, скрипт гейта, pre-commit и CI

- `shellcheck` добавлен в apt-список builder-стадии `src/Dockerfile`.
- Новый `scripts/run-shellcheck.sh` (по образу run-clang-tidy.sh): lint через builder-образ
  (`http-data-diod:builder`), режимы «changed»/`--all`, нулевой результат — `git status` change
  отсекается. Парсит `shellcheck -f json1`: findings уровня `error` — blocking (rc=1),
  предупреждения/info/style — печатаются неблокирующе (философия clang-tidy-гейта). Использует
  `$(getconf _NPROCESSORS_ONLN)` вместо `nproc` (нет на macOS).
- В `scripts/pre-commit.sh` добавлен шаг `run_shellcheck` до clang-tidy с опт-аутом
  `SKIP_SHELLCHECK=1` (зеркалит `SKIP_CLANG_TIDY`).
- В `.github/workflows/ci.yml` добавлен шаг `Shellcheck all shell scripts`
  (`bash scripts/run-shellcheck.sh --all`) после runtime-гейтов.

## Проверка

- `python3 scripts/lint-python.py` — 0 issues.
- `./scripts/ci-gate.sh offline` — включая новый lint-python-шаг — rc=0.
- `./scripts/run-shellcheck.sh --all` — блокирующих `error`-находок нет, rc=0 (остальное — неблокирующие
  pre-existing warning/info/style: SC2294 eval в cleanup.sh, SC2155 в profile.sh/resolve-crash.sh,
  SC2086 unquoted в health-check.sh и др.).
- `./rebuild-and-run.sh` — сборка зелёная (Dockerfile пересобран с shellcheck), стек поднят.
- `./health-check.sh all` — все сервисы OK, rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- `./scripts/ci-gate.sh all` — unit, offline, runtime (message_counter+dup-check, DB gateway e2e 7/7,
  golden 80/80, consistency offline+runtime 84/84/84/84) — rc=0.

## Побочные эффекты

- `src/run-with-valgrind.sh` и `src/run-with-heap-profiler.sh` теперь завершаются раньше при
  падении шагов (`set -euo pipefail`) — для диагностических скриптов это желаемое поведение.
- `scripts/run-shellcheck.sh` использует builder-образ; на свежем клоне pre-commit запустит
  `docker build --target builder` (тот же образ, что и для clang-tidy).
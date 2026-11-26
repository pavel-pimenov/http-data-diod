# Round 245 — Корневой .dockerignore (защита секретов в build context)

## Проблема
В корне репозитория **никогда не было `.dockerignore`** (проверено по git-истории —
файл не существовал с самого начала, это пробел, а не случайное удаление).
Sidecar-образ `grafana-sync` собирается с контекстом `$CI_PROJECT_DIR` (корень),
поэтому в build context / kaniko-кэш / слои образа попадали секреты:
- `certs_nats/` (ca.key, server.key)
- `.env` (TLS-настройки, NATS-токены)
- `.git`, `logs/`, `crash-dumps/`, `__pycache__/`, `.pyc`, build-каталоги

## Реализация
Создан `.dockerignore` в корне:
- Секреты/креды: `.env`, `.env.proxy.worker`, `certs/`, `certs_nats/`, `ssl/`, `*.key`, `*.pem`
- VCS: `.git`, `.github`
- Build-артефакты: `build/`, `build-*/`, `src/build*/`, `CMakeFiles`, `*.o/.a/.so/.pyc/.log`
- Runtime-данные: `logs/`, `crash-dumps/`, `spider-output/`, `heat-output/`, `coverage-report/`
- Локальные dev: `venv`, `.venv`, `.vscode`, `__pycache__/`, `node_modules/`
- `docs/` (не нужны сборке)
- Сохраняет `scripts/` (нужны sidecar) и `src/Dockerfile`

## Ключевое
- Build context sidecar сжался с всего репозитория до **240B**.
- Runtime-сервисы (контекст `./src`) не затронуты — у них свой `src/.dockerignore`.
- Перенос Dockerfile в корень — отдельная задача (round-246, по договорённости).

## Проверка
- Sidecar-образ собран (использован кэш, контекст 240B).
- `docker compose config` валиден.
- rebuild-and-run.sh зелёный.
- message_counter: no message loss.

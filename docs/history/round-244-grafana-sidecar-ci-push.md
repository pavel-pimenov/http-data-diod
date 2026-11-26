# Round 244 — Sidecar-контейнер генерации Grafana-дашбордов + пуш в реестр, удаление GitHub Actions

## Задача
Перенести генерацию Grafana-дашбордов (ранее запускалась на хосте из rebuild-and-run.sh)
в отдельный sidecar-контейнер. Grafana может находиться на другом хосте — URL задаётся
через `GRAFANA_URL`. Опубликовать sidecar-образ в Docker registry через GitLab CI.
Удалить GitHub Actions (проект использует только GitLab).

## Реализация
### `src/Dockerfile` — стадия `grafana-sync`
- База `ubuntu-base` + python3 + python3-pip + requests (через корпоративный PyPI-прокси).
- Копирует `generate-grafana-dashboards.py` и зависимости
  (`metrics-consistency-check.py`, `metrics_common.py`, `metrics-golden-check.py`).
- ENTRYPOINT: `python3 generate-grafana-dashboards.py --correct-dashboards`,
  `GRAFANA_URL` из env.
- Не является частью runtime-образа.

### `docker-compose.yml` — сервис `grafana-dashboards-sync`
- `image` + `build` (локально build, в prod pull из реестра).
- `restart: on-failure` (exit 0 после успешного синка — не зацикливается).
- Env: `GRAFANA_URL`, `GRAFANA_USER`, `GRAFANA_PASSWORD`.
- Скрипт сам ждёт готовность Grafana (retry) — нет жёсткой зависимости от локального grafana.

### `rebuild-and-run.sh`
- Убран блок генерации дашбордов на хосте (передано sidecar-контейнеру).
- Оставлен комментарий с on-demand командой для ручного синка.

### `.gitlab-ci.yml` — job `build_dashboards_sync`
- kaniko `--target grafana-sync`, контекст `$CI_PROJECT_DIR` (корень — стадия копирует
  `scripts/*.py` из корня, а не из `src/`).
- Пушит в `$IA_URSU_DOCKER_REPO_PATH/$CI_PROJECT_NAME-dashboards-sync:$IMAGE_TAG` + `:current`.
- Триггер: manual на тег/master (как runtime).

### Удалён `.github/workflows/ci.yml`
- Проект использует только GitLab CI, GitHub Actions не нужен.

## Проверка
- Стадия `grafana-sync` собрана; sidecar успешно сгенерировал дашборды в Grafana
  (`10/10 successful`, exit 0).
- `docker compose build grafana-dashboards-sync` — образ затегался
  `docker-registry.dp.nlmk.com/l2-dashboards-sync:latest`.
- `docker compose config` валиден, `.gitlab-ci.yml` YAML валиден.
- message_counter: no message loss.
- rebuild-and-run.sh зелёный.

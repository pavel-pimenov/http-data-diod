# Round 246 — Чистка устаревших упоминаний GitHub Actions + README-синхронизация (sidecar)

## Задача
После удаления GitHub Actions (round-244) в README и скриптах остались устаревшие
упоминания `.github/workflows/ci.yml` и «в CI (джобы clang-tidy/cppcheck/coverage)»,
которых больше нет. Также раздел «Дашборды» описывал генерацию Grafana на хосте,
тогда как с round-244 её делает sidecar-контейнер.

## Реализация
### `README.md`
- Раздел «Grafana-дашборды» переписан под sidecar: `grafana-dashboards-sync` —
  одноразовый контейнер, `GRAFANA_URL` может указывать на другой хост; ручной синк
  остался как on-demand команда.
- `rebuild-and-run.sh` синкает Perses и запускает sidecar для Grafana (не сам генерирует).
- Убраны упоминания `.github/workflows/ci.yml` (джоба coverage) — теперь честно:
  «замер покрытия запускается вручную `scripts/run-coverage.sh`, в CI не гоняется».
- clang-tidy/cppcheck — «в pre-commit и вручную, в CI не гоняются».
- Исправлены прочие «в CI» → локально/pre-commit.

### `scripts/ci-gate.sh`
- Комментарий: `.github/workflows/ci.yml` → `.gitlab-ci.yml`.

### `rebuild-and-run.sh`
- Комментарий про «GitHub Actions cache backend» → нейтральный «buildx cache backend».

## Ключевое
- При удалении GitHub Actions пропали CI-гейты coverage/clang-tidy/cppcheck
  (были только там). По решению пользователя они **не** добавляются в GitLab CI
  (нужен Docker daemon) — остаются в pre-commit и запускаются вручную.
- Перенос Dockerfile в корень отложен (отдельный фокусный раунд) — требует
  унификации контекстов сборки и правки гейтов.

## Проверка
- rebuild-and-run.sh зелёный (Perses 10/10).
- Sidecar `grafana-dashboards-sync` exit 0.
- message_counter: no message loss.

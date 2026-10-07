# Round 67: Perses — быстрый конвертер и каталог метрик в гейтах: offline-проверка, metrics-consistency, гигиена и README

## Мотивация

После интеграции Perses (round-66) конвертер дашбордов и его вывод остались вне гейтов:

- `scripts/generate-perses-dashboards.py --check` (offline-валидация нативного конвертера
  8 дашбордов) нигде не вызывался — сломанный конвертер прошёл бы незамеченным;
- каталог метрик в metrics-consistency считал «4 источника» (C++, Grafana, README, golden),
  а Perses-дашборды (третий набор PromQL) не сверялись — имей конвертер дрейф по метрикам,
  гейт бы не заметил;
- наш собственный wait-цикл в `rebuild-and-run.sh` давал warning shellcheck SC2034
  (переменная цикла не используется);
- README описывал только Grafana-доступ, про лёгкую альтернативу Perses — ничего.

## Изменения

### 1. `scripts/ci-gate.sh` — чеки Perses в offline-гейт

В `g_offline()` после lint-python добавлен шаг:

```bash
python3 scripts/generate-perses-dashboards.py --check
```

Это онлайн-независимая валидация нативного конвертера: структура panels/layouts, резолв
`$ref`, отсутствие пустых/заглушечных query и незаменимых `${...}` по всем 8 дашбордам.
Header-комментарий скрипта обновлён (offline-гейт теперь: lint-python, Perses --check,
metric-name consistency, env-var, docker-context). Так же, как lint-python, шаг не зависит
от контейнеров и идемпотентен.

### 2. `scripts/metrics-consistency-check.py` — Perses как пятый источник

- Добавлена загрузка `scripts/generate-perses-dashboards.py` и функция
  `perses_dashboard_metrics()`: для каждой пары в `DASHBOARDS` вызывается нативный
  `convert_dashboard_to_perses()`, из панелей извлекаются имена метрик запросов
  (с нормализацией `_bucket/_count/_sum` — как у Grafana-источника).
- Добавлено сравнение: `C++ registration == Perses dashboards`, а главное —
  `Grafana dashboards == Perses dashboards` (паритет PromQL между двумя системами).
  Сейчас наборы совпадают тождественно (конвертер не трогает строки запросов, только
  `${vm:regex}`→`$vm`), но гейт ловит любой будущий дрейф: кастомизацию запросов в
  конвертере, пропуск/добавление метрики.
- Сводка и финальная строка обновлены: `registered/dashboards/perses/readme/golden`,
  `OK: ... C++, dashboards, Perses, README and golden check`.

### 3. `rebuild-and-run.sh` — фикс shellcheck SC2034

Wait-цикл ожидания Perses переписан с `for i in $(seq 1 15)` на счётчик:

```bash
PERSES_TRIES=0
while [ "$PERSES_TRIES" -lt 15 ]; do
    if curl ...; then PERSES_READY=true; break; fi
    PERSES_TRIES=$((PERSES_TRIES + 1))
    sleep 1
done
```

Warning своего кода ушёл (проверено shellcheck -S warning: осталась только предсуществующая
SC2034 в графановском блоке строки 349, его не трогали по принципиальности scope'а раунда).
Заодно почищены отработавшие `/tmp/load*.log` от нагрузочного прогона round-66.

### 4. `README.md` — подраздел «Perses-дашборды (localhost:8089)»

После «Grafana-дашборды» добавлен раздел: URL `http://localhost:8089`, проект `l2`,
глобальный датасорс `prometheus` (HTTP-прокси на victoria-metrics), командный синопсис
(нативный синк / `--check` / `--output-dir` GitOps-экспорт), примечание что панели строятся
нативно из тех же определений и сверяются гейтом. Отдельно зафиксировано объяснение
«пустых» панелей: фиксированные окна `rate[1m]/[5m]`; при отсутствии события в окне линия
не рисуется — это не баг конвертера, поведение идентично Grafana.

## Проверка

- `python3 scripts/lint-python.py scripts/metrics-consistency-check.py scripts/ci-gate.sh scripts/generate-perses-dashboards.py` — 0 issues.
- `python3 scripts/generate-perses-dashboards.py --check` — 8/8 OK.
- `python3 scripts/metrics-consistency-check.py --offline` —
  `registered=84 dashboards=84 perses=84 readme=84 golden=84`,
  `Grafana dashboards == Perses dashboards (84 metrics)`.
- `shellcheck -S error rebuild-and-run.sh` — rc=0 (блокирующих нет); `-S warning` — осталась
  только предсуществующая SC2034 в графановском блоке (строка 349).
- `./rebuild-and-run.sh` — сборка зелёная, синк Perses 8/8.
- `./health-check.sh all` — rc=0; `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- `./scripts/ci-gate.sh all` — rc=0: unit, offline (включая новые Perses-шаги), runtime
  (message_counter+dup-check, DB gateway e2e 7/7, golden 80/80, consistency offline+runtime 84/84/84/84/84).

## Побочные эффекты / замечания

- Пустой результат «Grafana == Perses» сейчас тривиален (тождественный набор), но именно
  поэтому он ценен: регрессия конвертера по метрикам ловится сразу, а не на глаз.
- metrics-consistency в runtime-режиме теперь печатает и `perses=` в сводке — лог газе
  чуть информативнее, логика runtime-скрейпа не менялась.
- readme-раздел снабдил консистентностью с docker-compose (комментарий про `perses-data`
  и отсутствие docker-healthcheck остался там же).
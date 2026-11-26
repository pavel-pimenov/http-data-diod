# Round 251 — Баг: дублирующий panel id 10 в l2-db-spider (валидатор --check)

## Симптом
`python3 scripts/generate-grafana-dashboards.py --check` падал с:
```
ERROR - [l2-db-spider] validation errors (1):
  - duplicate panel id 10 (Запросы по статусу)
ERROR - --check failed
```

## Причина
В `create_spider_dashboard()` (l2-db-spider) два элемента получили одинаковый id 10:
- row-панель «Запросы и длительность» (`create_row_panel(..., 10, y)`),
- панель данных «Запросы по статусу» (`create_timeseries_panel(..., id=10, ...)`).

Валидатор `_validate_dashboard()` ловит дубликаты panel id.

## Фикс
Id панели данных «Запросы по статусу» изменён с 10 на 12 (id 11 занята панелью
«Длительность последнего опроса», id 12 свободен). Row-панель осталась с id 10
(по паттерну row-панелей кратно 10).

## Проверка
- `python3 scripts/generate-grafana-dashboards.py --check` → `--check passed`.
- Perses converter `--check`: 10/10.
- metrics-consistency `--offline`: 122 метрики согласованы.
- rebuild-and-run.sh зелёный (Perses 10/10).
- message_counter: no message loss.
- style-ошибки LONG100 в `heat-ai-server.py` — предсуществующие (round-247), не от этого round.

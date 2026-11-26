# Round 252 — Проверка env/metrics consistency (всё зелёное)

## Задача
Прогнать гейты согласованности окружения и метрик по AGENTS.md.

## Результат
### env-consistency (`scripts/env-consistency-check.py`)
- 105 C++ config-переменных, 195 compose-переменных.
- `OK: every C++ env var is wired in docker-compose.yml (105/105)` — висячих нет.
- 90 compose-переменных не читаются C++ (runtime/scripts только) — нормально
  (AI_*, GF_*, GLITCHTIP_*, GRAFANA_*, HEAT_*, POSTGRES_*, DB_SPIDER_* и др.).

### metrics-consistency (`scripts/metrics-consistency-check.py`)
- **--offline**: 122 метрики согласованы во всех источниках
  (C++ registration == dashboards == Perses == README == golden check).
- **--runtime**: `OK: metric names agree` (114/122 экспортируются, остальные
  lazy/label-dependent — нормально; http_pool метрики exported-elsewhere — нормально).

## Ключевое
Обе проверки зелёные, расхождений нет:
- висячих C++-переменных в docker-compose — 0;
- имена метрик согласованы в 4+ источниках (C++, дашборды, Perses, README, golden).

## Проверка
- rebuild-and-run.sh зелёный (Perses 10/10).
- message_counter: no message loss.

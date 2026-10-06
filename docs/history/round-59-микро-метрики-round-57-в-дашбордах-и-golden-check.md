# round 59: микро-метрики round 57 в дашбордах и golden-check

## Date: 2026-10-05

### Что сделано
- 6 метрик round 57 (`l2_proxy_task_queue_*`, `l2_proxy_nats_poll_*`) добавлены в панели генератора дашбордов и в `CATALOG` golden-check. Панели: «NATS poll attempts / запрос», «NATS poll attempt duration» (p50/p95/p99), «NATS poll retry wait», «Task queue wait (proxy)» (p50/p95/p99), «Task queue (enqueued/rejected)». Правка round 60: панели ошибочно попали в `nats-dashboard` и перекрывали панель id=22 — перенесены в `l2-proxy`.
- `generate-grafana-dashboards.py --check` теперь rc=0 (`All C++ metrics covered by dashboards`), `metrics-golden-check.py` — 75/75 семейств.
- Коммит `820fc99`.


# round 61: гейт метрик — обратное направление (дашборды ↔ живой /metrics) + юнит-тесты

## Date: 2026-10-05

### Что сделано
- **`--runtime` сверяет метрики в обе стороны**:
  1. каждый сервис обязан экспортировать то, что регистрирует (с учётом общих `l2_common` семейств — tracing/sentry лежат на всех экспозерах);
  2. каждая метрика из дашбордов обязана экспортироваться хотя бы одним сервисом — «панель с несуществующей метрикой» (ровно случай round 57) теперь ловится и на живых данных. Ленивые label-dependent семейства (`l2_proxy_per_client_id_*`, `l2_proxy_per_ip_*`, `l2_*_db_*`) по-прежнему уходят в `lazy`-notes, а не в fail.
- **Ошибка «стек не поднят» стала одной actionable-строкой**: если не отвечает ни один из трёх `/metrics` — `no /metrics endpoint is reachable — run ./rebuild-and-run.sh or use --offline`; при частичном падении роль с недоступным endpoint ругается отдельно.
- **Юнит-тесты `tests/test_metrics_consistency.py`** (41 тест, без сети и контейнеров): `normalize()`, скобочный парсер `collect_series_block()`, `cpp_registered_metrics()` на синтетических `src/` (включая Sentinel-фингерпринт и исключение `test_*.cpp`), `parse_exposition()`, `readme_metrics()`, `compare()`, `split_lazy()`, `golden_metrics()` и `runtime_check()` с инъекцией фейкового scrap-функции (все направления: успех, ghost-метрика дашборда, неэкспортируемое семейство, lazy, стек лежит целиком, пал один сервис).
- **Парсер вынесен в отдельную функцию** `parse_exposition(body)` — его теперь можно тестировать без HTTP.
- **`scripts/pre-commit.sh`**: новые шаги `run_unit_tests` (unittest discover до гейта метрик) и `run_metrics_check` (offline всегда + runtime при поднятом стеке). CI уже гонял `unittest discover` — тесты подхватились автоматически.
- **README** обновлён: раздел «Сверка имён метрик между источниками» описывает оба направления runtime-проверки и юнит-тесты.

### Проверка
- `python3 -m unittest discover -s tests`: rc=0, 41 тест.
- `python3 scripts/metrics-consistency-check.py --runtime`: rc=0 — обе стороны сходятся; dashboard-направление показало `every metric is exported by some service (72…79/84)`, остальное — только lazy-семейства.
- `SKIP_CLANG_TIDY=1 ./scripts/pre-commit.sh`: rc=0 (message_counter + health + юнит-тесты + metrics-гейт).
- Изменений в коде приложения нет — пересборка контейнеров не потребовалась.


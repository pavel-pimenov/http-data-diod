# round 60: гейт сверки имён метрик (C++ ↔ дашборды ↔ README ↔ golden-check)

## Date: 2026-10-05

### Проблема
- Round 57 добавил 6 метрик в C++, а в дашбордах и `metrics-golden-check.py` они не появились — расхождение обнаружилось только вручную. Round 59 закрыл конкретный случай, но механизма не было: причина в том, что `_collect_cpp_metrics()` в генераторе дашбордов читал **только** `src/app_context.cpp` (73 метрики), тогда как 11 метрик регистрируются в `src/proxy_init.cpp` (`DynamicLabeledFamily` + rate limiter). Из-за этого `--check` выдавал 11 ложных предупреждений «в дашбордах, но не в C++», и реальные расхождения тонули в шуме.

### Что сделано
- **Новый `scripts/metrics-consistency-check.py`** — сверяет четыре источника попарно и падает на любой асимметрии:
  1. регистрация в C++: `MetricsManager::create_*(\s*<registry>,\s*"l2_...")` + литералы внутри блоков `DynamicLabeledFamily<...>::Series>` (скобочный парсер, а не плоский regex), файлы `src/*.cpp|hpp` без `test_*`;
  2. PromQL в дашбордах: импортируется `generate-grafana-dashboards.py`, обходятся все `create_*_dashboard()`;
  3. каталог в README: строки таблиц с `` `l2_...` ``;
  4. `CATALOG` + `CONDITIONAL` из `metrics-golden-check.py`.
- **Два режима**: `--offline` (по умолчанию) — только разбор исходников, <1 с, без контейнеров и сети; `--runtime` — плюс скрап `/metrics` каждого сервиса (proxy `:19090`, worker `:19091`, l2-server `:19092`) и сверка экспортируемых имён с регистрациями. Семейства, чьи метки заполняются только под нагрузкой (`l2_proxy_per_client_id_*`, `l2_proxy_per_ip_*`, `l2_*_db_*`), попадают в отчёт как `lazy`, а не `missing`; имена из `l2_common` (tracing/sentry) лежат на всех экспозерах и учитываются через `SHARED_PREFIXES`.
- **Найдено и исправлено реальное расхождение**: 5 семейств DB Gateway (`l2_proxy_db_requests_total`, `l2_proxy_db_request_duration_seconds`, `l2_proxy_db_nats_request_duration_seconds`, `l2_worker_db_requests_total`, `l2_worker_db_query_duration_seconds`) были зарегистрированы в C++, попали в дашборды и README, но отсутствовали в golden-check. Добавлены в `CONDITIONAL` (метки `db`/`type`/`status` — prometheus-cpp не эмитит ничего до первой комбинации меток, плюс нужен живой DB).
- **Гейты**: `scripts/pre-commit.sh` (новая функция `run_metrics_check`, offline всегда + runtime при поднятом стеке, пропуск `SKIP_METRICS_CHECK=1`) и `.github/workflows/ci.yml` (шаг после golden-metrics/smoke).
- **Документация**: новый раздел README «Сверка имён метрик между источниками» + правило в `AGENTS.md`.

### Попутно исправлено в генераторе дашбордов
- `_collect_cpp_metrics()` больше не читает только `app_context.cpp`: разбор делегирован парсеру из `metrics-consistency-check.py` (охватывает `proxy_init.cpp`, отбрасывает не-метрические литералы вроде `l2_server_call_error`). Ложные 11 предупреждений исчезли, `--check` видит все 84 метрики.
- **Наложение панелей**: панели round 59 (NATS poll / task queue, id 50/25/26/27/28) попали в `nats-dashboard` и перекрывали панель id=22 «NATS события подключения во времени». Перенесены в `l2-proxy` (их правильное место) как новая секция «NATS poll и очередь задач» (row id=80, панели 81–86), раскладка 6×8 с последующим сдвигом `y` — перекрытий нет.
- Попутно устранены два доставшихся с прошлых раундов наложения: в `l2-server` row id=90 и панели 200/201 вставали прямо на панель 21 «Длительность запроса» (добавлен пропущенный `y += 8`), в `l2-slo-tracking` панели 43/44 стояли на 41/42 (добавлен `y += 6`). Проверено попарным расчётом прямоугольников: во всех 8 дашбордах 0 наложений и 0 дубликатов id.

### Стоимость
- Offline — regex по ~10 файлам, 0.3–0.6 с. Runtime — 3 HTTP-запроса к локальным портам, <1 с. Оба укладываются в pre-commit без заметного замедления; проверка ловит рассинхрон (проверено временной инъекцией `l2_proxy_drift_probe_total` в `app_context.cpp` — offline-режим упал на всех трёх источниках сразу).

### Проверка
- `python3 scripts/metrics-consistency-check.py --offline`: rc=0, `registered=84 dashboards=84 readme=84 golden=84`.
- `python3 scripts/metrics-consistency-check.py --runtime`: rc=0 — proxy 45 метрик, worker 31, l2-server 18; lazy — только label-зависимые семейства.
- `python3 scripts/generate-grafana-dashboards.py --check`: rc=0, `l2-proxy` 55 панелей / 44 метрики, `nats-dashboard` 21 / 5. `python3 scripts/metrics-golden-check.py`: rc=0 (75/75).
- Раскладка всех 8 дашбордов проверена попарно: 0 наложений, 0 дубликатов id.
- `SKIP_CLANG_TIDY=1 ./scripts/pre-commit.sh`: rc=0 (включая новый metrics-гейт).
- Изменений в коде приложения нет (только скрипты/документация/CI) — пересборка контейнеров не потребовалась.


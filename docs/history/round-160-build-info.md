# round 160 — build info: `l2_common_build_info{version,mode}` (P1 #8 из TODO)

## Что сделано

Стандартная для сервисов метрика «какой это релиз», которой в проекте не было: чтобы узнать
версию, приходилось идти в логи `Logger::info("L2-Proxy version: {}")`, а на дашборде сверить
деплой с метриками было нечем.

```
l2_common_build_info{version="<git-describe>", mode="proxy|worker|server"} 1
```

Регистрация — в `AppContext::init_common()` (`src/app_context.cpp`), в **общем** реестре
`l2_common`, рядом с tracing/sentry: этот реестр подмешивается к реестру каждого режима, поэтому
одна и та же серия есть в `/metrics` l2-proxy (`19090`), l2-worker (`19091`) и l2-server
(`19092`) — версия видна на любом экспозере, без дублирования регистрации по режимам.

```cpp
MetricsManager::create_gauge_family(m_common_registry, "l2_common_build_info",
                                    "Build information (value is always 1)")
    .Add({{"version", g_l2_proxy_version}, {"mode", m_config.m_app.m_mode}})
    .Set(1);
```

Версия берётся из `g_l2_proxy_version` (`src/version.cpp`, значение — `VERSION` из CMake, туда же
его клали и в Sentry-релиз `m_sentry.m_release`), режим — из уже загруженного к этому моменту
`m_config.m_app.m_mode`. Объявление `extern const char *g_l2_proxy_version;` добавлено локально,
как это уже сделано в `src/config.cpp`: единственный заголовок с этим символом — `crash_handler.hpp`,
а тянуть в `app_context.cpp` всю Sentry-машинерию ради одной строки не хочется.

## Отличия от плана в TODO

- **Добавлена метка `mode`** (в TODO были только `version`). В одном VictoriaMetrics серии трёх
  сервисов с одинаковой версией слились бы в одну (проект и так срежет их по инстансу `vm`, но
  режим отличает proxy/worker/server и при одинаковой версии, и в агрегатах без `vm`).
  Значений ровно три, кардинальность фиксированная.
- **`uptime` не делали.** В TODO пункт назван «Build info и uptime», но отдельный
  `l2_common_uptime_seconds` дублировал бы `process_start_time_seconds` из стандартного
  экспортёра процессов: это постоянный шум в каталоге без новой информации (время старта уже
  есть, uptime = now − start).
- **Gauge-семейство, а не counter.** Значение всегда `1`; метрика нужна как «носитель меток» для
  джойнов в панелях/алертах (`version` рядом с любым rate'ом), а не как счётчик.

## Дашборд

На дашборде L2 Прокси добавлена панель **206 «Версия сборки»** (`create_stat_panel`,
`color_mode="none"`, легенда `{{version}} ({{mode}})`) — рядом с панелью «Готовность
(health/ready)» в блоке статуса. Панель именно в дашборде прокси, а не во всех трёх: метрика
общая, и для проверки «метрика не потерялась» достаточно одного места, а дублировать одну и ту же
stat-панель в трёх дашбордах — шум. Правка — снова target-only (панель добавляется, но у неё
единственный target на существующей общей метрике), то есть ровно тот случай, который генератор
перестал терять после починки в round 159 (`get_dashboard_exprs()`).

## Соответствие четырём источникам

Метрика добавлена во все четыре списка, которые сверяет
`scripts/metrics-consistency-check.py`: регистрация в C++ (`app_context.cpp`), каталог в
`README.md` (новый мини-раздел «Идентификация сборки (общий реестр `l2_common`)»), `CATALOG` в
`scripts/metrics-golden-check.py` и PromQL в `scripts/generate-grafana-dashboards.py`
(панель 206). Perses-дашборды и дашборды Grafana генерируются из той же фабрики, так что
расхождения там быть не может. Всего метрик в четырёх источниках стало **91**.

## Тесты

- Имя `l2_common_build_info` добавлено в `common_l2_families()` (`src/test_app_context.cpp`) —
  за счёт этого существующий хелпер `require_common_registry_only_on_common()` сразу проверяет
  для **всех трёх режимов** (proxy/worker/server), что семейство есть в общем реестре и
  **не** протекло в реестры режимов.
- Новый кейс `AppContext: build info carries the version and the mode` проверяет, что в семействе
  ровно одна серия, её значение равно `1`, а метки — это непустой `version` и `mode`, равный
  текущему режиму из `MODE`. Так тест ловит и потерю метки, и случайное значение не `1` (иначе
  метрика перестала бы быть «информационной» и сломала бы джойны).

## Проверка

- `metrics-consistency-check.py --offline` — 91 метрика во всех четырёх источниках; runtime-гейт —
  `registered=91 dashboards=91 perses=91 readme=91 golden=91`, то есть согласованы и живые
  `/metrics`/VM, не только исходники.
- `generate-grafana-dashboards.py --check` — `--check passed`; `generate-perses-dashboards.py
  --check` — `Offline check passed (8/8)`; `lint-python.py` — 0 замечаний.
- Живой накат дашбордов: 7 дашбордов «up to date» + **1 обновлён** (L2 Прокси, новая панель 206) —
  то есть idempotent-проверка из round 159 действительно видит правку, а повторный прогон не
  делает лишний PUT. Parity Grafana↔Perses — `OK` (панель есть в обоих).
- Сборка: `2853 assertions in 594 test cases` (+1 кейс — новый тест build info), `945 assertions in
  125 test cases`, `rebuild_rc=0`; health ✅, smoke ✅ (`Failed requests: 0`).
- `ci-gate.sh runtime` с поднятым профилем `databases` (rc=0): DB Gateway e2e 7/7, golden
  `87/87 families` c новой группой **`COMMON=1`** (семейство реально скрейпится из VM:
  `l2_common_build_info{version=…,mode=…}`), precision parity — `OK`.
- `run-cppcheck.sh` — rc=0; `run-clang-tidy.sh --all` — 47 файлов, «no errors or warnings in
  project files».
- `run-coverage.sh` — rc=0, пер-файловых регрессий нет, production 6613/7226 = **91.52%**,
  полный scope 15627/16558 = 94.38%, ветви 30453/74009 = 41.15%, production-ветви
  7083/13284 = 53.32%.

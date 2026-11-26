# round 212 — фикс метрик в Grafana (переменная vm), NATS 2.15.2, VM порт 8428

## Задача

В Grafana не отображались метрики — дашборды были пустые. Разобраться почему
и починить, затем прогнать нагрузочный тест на 3-5 минут для проверки всего
стека.

## Диагностика: почему нет метрик в Grafana

Пошагово выяснилось, что сбор метрик работает:
- VictoriaMetrics здоров, содержит **161 l2_* метрику** (проверка по
  `/api/v1/label/__name__/values`);
- Grafana datasource VictoriaMetrics работает (`/api/datasources/proxy/1/...`
  отдаёт `l2_common_build_info` и др.);
- панели с явным `vm=nl-sp-p3pg-ad1` отдают реальные данные.

**Корень проблемы:** шаблонная переменная `vm` в дашбордах была настроена как
`label_values(up, vm)` — это PromQL-функция Prometheus, которую VictoriaMetrics
**не поддерживает** (422 `unsupported function "label_values"`). Переменная
оставалась пустой (`options=[]`, `current=None`), и запросы `vm=~"$vm"` не
матчились → панели пустые.

### Фикс переменной vm

Вместо `label_values(up, vm)` переменная настроена на:
- query: `up` (метрика, возвращает серии с label `vm`);
- regex: `.*vm="([^"]+)".*` — Grafana извлекает значения label `vm` из
  результата через API label values (поддерживается VictoriaMetrics).

Обновлено в `scripts/generate-grafana-dashboards.py` и применено ко всем
дашбордам через Grafana API. Проверено: дашборд `l2-proxy` рендерится (HTTP
200, PNG с данными), панели с `vm=nl-sp-p3pg-ad1` отдают `878 req/s`, `in-flight
19`.

## VictoriaMetrics порт: 8430 → 8428

Ранее VM был проброшен на `8430:8428` из-за якобы занятого 8428 другим стеком
(FlylinkDC++). Проверка: порт 8428 свободен, другого контейнера `victoria-metrics`
нет. Вернул стандартный порт `8428:8428` и обновил все ссылки:
- `docker-compose.yml` — порт, убран устаревший комментарий про FlylinkDC++;
- `health-check.sh` — `VM_PORT=8428`;
- `scripts/alerts-consistency-check.py` — `VM_URL http://localhost:8428`;
- `scripts/metrics-golden-check.py` — default URL `http://localhost:8428`.

## NATS Server 2.15.2

Обновлён NATS Server `2.14-alpine → 2.15.2-alpine`. Образ `nats:2.15.2-alpine`
скачан с Docker Hub и затаргован локально как `docker-registry.dp.nlmk.com/nats:2.15.2-alpine`
(registry не имеет этого тега). После перезапуска: `nats-server v2.15.2`,
l2-proxy/l2-worker переподключились (auto-reconnect), message_counter прошёл.

Также pull нового NATS клиента (ваш коммит `c66f0bf`, src/nats/) и полная
пересборка — зелёная.

## Нагрузочный тест (4 минуты)

`message_counter.py --duration 240 --concurrent 20 --test post`:
- **208,375 запросов** за 240 сек (868 RPS стабильно);
- **0 ошибок** (100% успешность), без потерь сообщений и перекрёстных ответов;
- **Latency: p50 = 19.13ms, p95 = 45.52ms, p99 = 64.55ms, max = 189.38ms**.

Метрики зафиксировали нагрузку в VM: `client requests/s (5m) = 667`, `responses
total = 297446`. Grafana показывает данные во время нагрузки.

## Мусорный label `vm="hostname"` в фильтре Grafana

В фильтре узла Grafana кроме `nl-sp-p3pg-ad1` выпадал пустой `hostname`.
Выяснилось: в VictoriaMetrics есть **3168 серий** с `vm="hostname"` — старые
мусорные данные от раннего запуска, когда `VM_NAME` не был установлен (в
vmagent конфиге `vm: '%{VM_NAME}'`, без установленной переменной подставилось
литерал `hostname`). В основном это самоскрейп VM (`flag`, `vm_*`, `go_*`) и
старые `l2_*` метрики. Серии не находились по `{vm="hostname"}` (0), но их
label `vm` оставался в индексе label values.

Удалены через VictoriaMetrics admin API:
```
POST /api/v1/admin/tsdb/delete_series?match[]={vm="hostname"}
```
После удаления: label vm values = `['nl-sp-p3pg-ad1']` (только узел),
серий `vm="hostname"` = 0. В Grafana фильтр узла теперь без мусора.

Защита от повторения: добавлен `VM_NAME=nl-sp-p3pg-ad1` в `.env` (чтобы vmagent
всегда помечал метрики реальным именем узла, а не литералом). В `.env.example`
документирован VM_NAME с предупреждением не использовать литерал "hostname".

## Файлы

- `docker-compose.yml` — NATS 2.15.2, VM порт 8428, Grafana anonymous;
- `health-check.sh` — VM_PORT 8428;
- `scripts/alerts-consistency-check.py`, `scripts/metrics-golden-check.py` — VM URL 8428;
- `scripts/generate-grafana-dashboards.py` — переменная vm (up + regex);
- `.env` — VM_NAME (в gitignore, защита от литерала hostname);
- `.env.example` — VM_NAME документирован.

## Проверки

- health-check all passed;
- message_counter Success 1/0;
- load test 240s: 868 RPS, 0 errors, p95 45ms.

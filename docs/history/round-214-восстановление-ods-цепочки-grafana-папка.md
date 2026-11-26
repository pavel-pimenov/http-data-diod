# round 214 — восстановление ODS-цепочки (DataHubRobots), порты 29*, Grafana-папка

## Задача

Тесты ODS (`ods-test-query-datahub.py`, `ods-test-stress-datahub.py`,
`ods-test-query-datahub-diag.py`) перестали работать: возвращали пустоту
(`value_return: 0`) или ошибки. Причина — несколько наложившихся проблем:
порт nginx сменился (7777 → 29003), из конфига уехали ODS-апстримы, а рабочий
эндпоинт `DataHub/DataHubSrv` на самом деле называется `DataHubRobots/DataHubSrv`.

## Порт 7777 → 29003

В round-209 порты `29xxx` стали рабочими/данными, а `19xxx` — метрики. nginx
переехал с `7777:80` на `29003:80`. Все ODS-скрипты продолжали стучаться на
`7777` (устаревший nginx).

Обновлены:
- `ods-test-query-datahub.py`: `:7777` → `:29003` во всех URL.
- `ods-test-query-datahub-diag.py`: `port = 7777` → `29003`.

## Возвращение ODS-апстримов

В коммите `e44fa7e` из docker-compose.yml убрали `L2_SERVER_URLS` с
ODS-апстримами:
```
L2_SERVER_URLS=["https://odskc2.nlmk:8181/DataHub/DataHubSrv", "https://odskc2.nlmk:8181/GantPool/GetGantDescr"]
```
заменив на `L2_SERVER_URLS=${L2_SERVER_URLS:-}` (пусто). При пустом значении
воркер маршрутизирует все запросы на локальный стаб `l2-server:8088`, который
отвечал `{"value_return": 0}` — реальный ODS-апстрим не достигалcя.

Возвращено в `.env` (не в git):
```
L2_SERVER_URLS=["https://odskc2.nlmk:8181/DataHubRobots/DataHubSrv", "https://odskc2.nlmk:8181/GantPool/GetGantDescr"]
```
`.env.example` задокументирован.

## Эндпоинт DataHub → DataHubRobots

`/DataHub/DataHubSrv` на ODS-апстриме отвечал «Empty reply from server»
(сервер сразу закрывал соединение). Правильный рабочий эндпоинт —
`/DataHubRobots/DataHubSrv`, возвращает реальные данные (HTTP 200, ~5 КБ).

Обновлены:
- `.env` / `.env.example`: `DataHub/DataHubSrv` → `DataHubRobots/DataHubSrv`.
- `nginx.conf`: location-правило матчит и `DataHubRobots/DataHubSrv`
  (и оставлен `DataHub/DataHubSrv` для обратной совместимости).
- `ods-test-query-datahub.py`, `ods-test-stress-datahub.py`: URL эндпоинта.

## Auth-заголовки

ODS-эндпоинт требует Basic auth (`login1:pass1`). Без него ODS-nginx отдаёт 401.
Добавлены заголовки во все HTTP-скрипты:
- `Authorization: Basic ...` (base64 `login1:pass1`);
- `X-DataHub-Client-Id: 1000023-6666666666666`.

Воркер форвардит эти заголовки апстриму (значения чувствительных заголовков
только редактируются в логах, но передаются дальше).

## Diag-скрипт: raw-socket → HTTP

`ods-test-query-datahub-diag.py` использовал сырой TCP-сокет с 4-байтовым
length-prefix (`struct.pack("!I", len) + payload`) — протокол, несовместимый с
nginx (говорит только HTTP). Скрипт не работал через цепочку. Переписан на
HTTP POST к `/DataHubRobots/DataHubSrv` через цепочку с auth и
`X-DataHub-Client-Id` заголовками.

## Grafana-дашборды в папку l2-http-diod

Дашборды сохранялись в корень Grafana (General). Теперь все генерируемые
дашборды лежат в папке `l2-http-diod`.

В `scripts/generate-grafana-dashboards.py`:
- константа `GRAFANA_FOLDER_NAME = 'l2-http-diod'`;
- `GrafanaAPI.ensure_folder()` — находит или создаёт папку через `/api/folders`,
  идемпотентно;
- `save_dashboard()` — добавляет `folderId` в payload;
- `correct_dashboard_panels()` — проверяет, что дашборд в правильной папке,
  перемещает при необходимости;
- `main()` — создаёт папку сразу после подключения к Grafana.

## README: сегмент ОДС КЦ-1/2, ревизия метрик, реорганизация схем

В README-диаграммах `DataHub-GantPool` вынесен из технологического сегмента l2
в отдельный сегмент **«ОДС КЦ-1/2»** (по аналогии с сегментом баз данных).
Обе mermaid-диаграммы обновлены: в первой (flat `flowchart TD`) добавлен
`subgraph ODS_SEG`, во второй (`flowchart LR` с сегментами) — `subgraph ODS_NET`.

Ревизия метрик README: `metrics-consistency-check.py --offline` подтверждает,
что все 117 метрик отражены в README-каталоге и согласованы с C++, дашбордами,
Perses и golden check.

Сегментная схема («Сетевая схема с разделением сегментов») поднята выше и
показывается перед «Визуальной схемой взаимодействий». Визуальная схема
актуализирована по `docker-compose.yml`: добавлены vmalert, victoria-metrics
(переименован из prometheus), glitchtip, glitchtip-db, grafana, perses,
swagger-ui, nats-gui и их связи (метрики VM → Grafana/Perses/vmalert, алерты,
Sentry-DSN → proxy/worker, glitchtip → glitchtip-db, nats-gui → nats,
swagger-ui → nginx).

## Удаление live-чейна FlylinkDC++

Живая цепочка FlylinkDC++ (ptokax 37014/37015) переехала в другой проект и из
этого репозитория удалена полностью:
- удалены `docker-compose.live.yml` и `scripts/live-chain-check.py`;
- README: убрана секция «Живая цепочка FlylinkDC++», разделы о health-check и
  cron подняты с `###` до `##`;
- docker-compose.yml / .env.example: комментарии обобщены (убраны
  FlylinkDC++/ptokax/PTOKAX/37014/37015);
- C++-комментарии (l2_worker.cpp, request_handler.cpp, utf8_utils.hpp и тесты)
  обобщены на «бинарный/легаси апстрим» вместо FlylinkDC++/PTOKAX — логика не
  менялась.

## Проверка

- `ods-test-query-datahub.py`: GetHeatData → HTTP 200 (~5 КБ данных), GantPool →
  HTTP 200 (~325 КБ).
- `ods-test-stress-datahub.py`: 3/3 OK, 0 ошибок.
- `ods-test-query-datahub-diag.py`: HTTP 200, реальные данные.
- Grafana: все 9 дашбордов в папке `l2-http-diod` (`folderTitle: l2-http-diod`),
  папка создаётся идемпотентно.
- `ci-gate.sh offline`: Python-style, metric/alert/env/docker-gates — rc=0.

Примечание: `message_counter.py` (полагается на стаб l2-server) теперь падает,
потому что `L2_SERVER_URLS` указывает на ODS-апстрим — это ожидаемое следствие
продакшен-конфигурации, а не регрессия.

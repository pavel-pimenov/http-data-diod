# Round 222 — Починка GetHeatData через прокси-цепочку + сервис перебора плавок heat-scraper

## Задача
1. Понять и починить запрос GetHeatData, который раньше возвращал заглушку
   `{"value_return": 0}` вместо реальных данных о плавке.
2. Создать сервис (новый docker-образ с Python), который перебирает плавки от
   заданного номера вниз на N и сохраняет ответ каждой в отдельный JSON-файл
   (временные, в git не коммитятся).

## Диагноз проблемы
Запрос шёл через прокси-цепочку `nginx(29003) → l2-proxy → NATS → l2-worker → l2-server`.
- l2-server — тестовый стаб: `server_handler.cpp` читал ключ `value` из JSON-запроса
  и возвращал `value_return: value` (при отсутствии `value` — всегда 0). GetHeatData
  не обрабатывался.
- В `.env` `L2_SERVER_URLS` был закомментирован → worker маршрутизировал
  /DataHubRobots/DataHubSrv на стаб, а не на внешний ODS.
- Клиент не передавал Basic-auth и X-DataHub-Client-Id.

## Реализация
### Починка цепочки прокси
- `.env`: раскомментирован и задан `L2_SERVER_URLS` на внешний ODS:
  `["https://odskc2.nlmk/DataHubRobots/DataHubSrv", "https://odskc2.nlmk/GantPool/GetGantDescr"]`
  → worker проксирует /DataHubRobots/DataHubSrv на реальный DataHub.
- Клиент передаёт `Authorization: Basic` и `X-DataHub-Client-Id`; proxy и worker
  пробрасывают их (не входят в skip-список заголовков).
- Проверено: запрос через nginx:29003 теперь возвращает реальный JSON с
  `result.data.processes` (температуры, химсостав, налив/УДЧ и т.д.).

### `scripts/heat_scraper.py`
- Перебирает плавки от `start` вниз на `count` (CLI/env: `--start`, `--count`).
- Для каждой шлёт POST `GetHeatData` через прокси-цепочку, сохраняет ответ в
  `heat_<номер>.json` (поле `response` + heat_no/http_status/scraped_at).
- Запись `_scrape_summary.json` со сводкой (total/ok/error).
- Креды из env: `HEAT_SCRAPE_USER/PASSWORD/CLIENT_ID`; URL — `HEAT_SCRAPE_URL`.
- Только встроенный urllib (без внешних зависимостей).
- Важно: `id` в JSON-RPC — целое число (строка давала JSON-RPC ошибку).

### `src/Dockerfile` — stage `heat-scraper`
- На базе `pdf-report` (python3 уже установлен), копирует `heat_scraper.py`,
  ENTRYPOINT = python3 heat_scraper.py. NOT part of runtime images.

### `docker-compose.yml` — сервис `l2-heat-scraper`
- Запуск по demand (`docker compose run --rm l2-heat-scraper`), restart: "no".
- Volume `./heat-output:/work/heat-output`; env HEAT_SCRAPE_*.
- Параметры start/count — env/CLI, позже вынесем в GUI.

### `.env` (git-ignored) — секреты ODS
- `HEAT_SCRAPE_USER=login1`, `HEAT_SCRAPE_PASSWORD=pass1`,
  `HEAT_SCRAPE_CLIENT_ID=1000023-6666666666666`, START=23143, COUNT=100.

### `.gitignore`
- Добавлен `heat-output/` (временные файлы плавок).

### `.env.example`
- Задокументированы HEAT_SCRAPE_* переменные.

## Проверка
- `docker compose config` OK; `docker compose build l2-heat-scraper` — образ собран.
- `docker compose run --rm l2-heat-scraper` — собрал 100 плавок (23143..23044) в
  `heat-output/` (100 файлов + сводка). OK=0, error=100 — плавки 23xxx НЕ существуют
  (реальные плавки ODS ~400..900; 23143 пустой → «Запрос вернул пустые данные»).
- На хосте (с env) проверено: 107 → OK (2 processes), 23143 → empty/error.

## Замечание
Плавки в диапазоне 23143 вниз (23xxx) — пустые: такие номера не существуют на ODS.
Реальные плавки ~400..900 (400/440/500/600/700/800 → OK). Стартовый номер/диапазон
позже вынесем в GUI сервиса для корректировки.

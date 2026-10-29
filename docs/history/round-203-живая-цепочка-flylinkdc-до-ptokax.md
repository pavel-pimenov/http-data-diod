# round 203 — живая цепочка FlylinkDC++: nginx 37015 → диод → ptokax 37014

## Что было не так

Диод до этого раунда существовал только как стенд с встроенной заглушкой
`l2-server`: рабочая цепочка `клиент → nginx:37015 → ptokax:37014` шла мимо него.
Попытка встроить диод в реальный трафик упиралась в пять независимых проблем:

1. **Хост-порт 8888 занят хабом.** `ptokax` публикует свой порт, а прокси диода
   пробрасывал наружу тот же `8888:8888` — поднять диод рядом с хабом было
   нельзя.
2. **Реальный трафик — не JSON.** FlylinkDC++ на `/fly-stat`, `/fly-zget`,
   `/fly-download`, `/fly-error-sql` присылает **zlib-сжатые** тела
   (`0x78 0xDA …`). `RequestHandler::handle_post()` отвергал всё, что не
   проходит `nlohmann::json::accept()`, с `400 Invalid JSON in request body`:
   то есть диод физически не мог пропустить ни один «настоящий» запрос.
3. **Конверт NATS не переживает бинарь.** Даже если пропустить тело, положить
   его в JSON-конверт нельзя: `std::string` с невалидным UTF-8 не сериализуется
   (`type_error.316`), а 8 MiB тело (лимит nginx/хаба) не влезало в дефолтный
   `max_payload` NATS в 1 MiB.
4. **PTOKAX закрывает keep-alive без `Connection: close`.** Переиспользованный
   сокет из пула оказывался мёртвым, httplib отдавал `Error::Connection`,
   прокси отвечал 500, а после пяти подряд открывался circuit breaker и
   начинался шквал 503.
5. **Шум и ложные тревоги.** Включённый шлюз БД в живой цепочке (где узла БД
   нет) бесконечно переподключался к `postgres:5432` — две строки ошибки
   каждые ~0.5 с. Per-client `RateLimiter initialized` писался в INFO на каждый
   новый IP. Golden check требовал семейства БД безусловно. А имена
   `victoria-metrics`/`perses` и порт 8428 уже были заняты стеком хаба.

## Что сделано

### Live-override: `docker-compose.live.yml`

Запуск — `COMPOSE_FILE=docker-compose.yml:docker-compose.live.yml ./rebuild-and-run.sh`.
Базовый `docker-compose.yml` остаётся стендом с заглушкой, override добавляет:

- `l2-proxy` и `l2-worker` подключаются к **внешней** сети
  `flylinkdc-hub_default` (владелец — стек хаба, поэтому `down` диода не
  инвалидирует сетевую привязку nginx хаба);
- `l2-worker`: `L2_SERVER_PROTOCOL/HOST/PORT/URLS = http/ptokax/37014/["http://ptokax:37014"]`;
- `DB_QUERY_ENABLED=false` (прокси и воркер) и `DB_POSTGRES_ENABLED=false`
  (воркер) — в живой цепочке узла БД нет;
- `MAX_RETRIES=2`: `MAX_RETRIES` считает **все** попытки, поэтому 1 превращала
  мёртвый keep-alive сокет в 500, а 2 — в повтор на свежем сокете (провалившийся
  клиент инвалидируется и пересоздаётся);
- `PROXY_RAW_BODY_PASSTHROUGH=true` (в базовом файле флаг по умолчанию `false`).

### Сырые тела: флаг, base64-конверт, декодирование

- `PROXY_RAW_BODY_PASSTHROUGH` (`Config::Proxy::m_raw_body_passthrough`,
  `config.cpp`) — при `true` `handle_post()` не отвергает не-JSON тело:
  пропускает его дальше вместе с исходным `Content-Type`.
- `prepare_request_data()` (`request_data_preparer.cpp`): JSON-тело идёт как
  раньше, не-JSON — `base64::to_base64(body)` плюс маркер
  `body_is_binary: true` (`json_utils.hpp`, `NatsContract::kBodyIsBinary`).
- `WorkerRequestParser` (`worker_request_parser.hpp`) декодирует base64 обратно
  в байты **до** вызова хаба, поэтому тело доходит байт-в-байт.
- `set_response_content()` (`response_builder.cpp`) сохраняет `Content-Type`
  апстрима (легаси test-port отвечает `text/html` на неизвестных путях) вместо
  жёсткого `application/json`.
- `handle_duplicates()` дампит отчёт с
  `error_handler_t::replace`: в отчёт попадают сэмплы бинарных тел, и обычный
  `dump(2)` выбрасывал исключение прямо из обработчика.

### Конфигурация стека

- `nats/nats.conf` + монтирование в `nats-server`: `max_payload: 16777216`
  (у nats-server нет CLI-флага для этого лимита), `jetstream.store_dir: /data`;
  порты и `-js`/`-sd` ушли в файл, auth/TLS остались в командной строке, так
  как зависят от env.
- `l2-proxy` публикуется как `8890:8888` (внутри сети порт прежний — на него
  смотрят nginx и воркер); `L2_SERVER_*` в базовом файле стали
  `${…:-default}`.
- `victoria-metrics` → `container_name: l2-victoria-metrics`, хост-порт
  `8430:8428`; `perses` → `container_name: l2-perses`. Имена в сети
  (`victoria-metrics:8428`) не менялись — на них смотрят vmagent/vmalert/Perses.
- Скрипты и документация переведены на новые хост-порты: `health-check.sh`,
  `message_counter.py`, `load_test.py`, `rate_limit_test.py`,
  `fault_tolerance_test.py`, `docs/http-db-gate-example.md`, README;
  `metrics-golden-check.py` и `alerts-consistency-check.py` — на
  `http://localhost:8430`.

### Наблюдаемость: без ложных тревог

- `RateLimiter initialized` понижен с INFO до DEBUG: лимитер создаётся лениво
  на каждый новый IP, и на реальном трафике строка доминировала в логе прокси
  (глобальный лимитер по-прежнему пишет INFO один раз при старте).
- `scripts/metrics-golden-check.py`: пять семейств
  `l2_worker_db_pool_*`/`l2_worker_db_gateway_ready` перенесены из `CATALOG` в
  `CONDITIONAL` — их создаёт DB-обработчик, а при `DB_QUERY_ENABLED=false` его
  нет и prometheus-cpp ничего не экспортирует; `tests/test_metrics_consistency.py`
  (`DB_FAMILIES`) дополнен теми же именами.

### Вне репозитория

В `flylinkdc-hub/nginx.conf` (стек хаба) upstream test-port переведён с
`proxy_pass http://ptokax:37014` на `http://l2-proxy:8888` с
`resolver 127.0.0.11 valid=10s`, чтобы пересоздание контейнера диода не требовало
ручного рестарта nginx. Откат — вернуть прямой upstream.

## Проверка

- **Прозрачность цепочки**: собственный чекер (прямой запрос в `ptokax:37014`
  против запроса через `nginx:37015`) на пяти сценариях — plain text, бинарное
  тело, zlib-JSON, тело 1 MiB, `/fly-test-port` — **ALL OK**: статус, набор
  заголовков контента и тело совпадают побайтово.
- **nginx 37015**: единственный всплеск 502 был в окне 11:15–11:34 UTC, пока
  прокси пересоздавался (`connect() failed (111: Connection refused)` на
  `172.18.0.7:8888`); после 11:34 — только 200, ни одного 5xx.
- **Шум воркера**: после `DB_QUERY_ENABLED=false` переподключения к
  `postgres:5432` прекратились, лог на простое чистый.
- **Golden check** на живом стеке без флагов: `OK: golden metrics set complete
  (102/102 families)`; `metrics-consistency-check.py --offline` — 117/117.
- **Стенд не сломан**: полный `./rebuild-and-run.sh` на дефолтном стеке —
  сборка, юнит-тесты (3037 assertions / 624 cases + 946 / 125), health-check,
  golden check — зелёные; `./scripts/ci-gate.sh unit` и `offline` — rc=0.

## Ограничения (честно)

- Live-режим требует правки конфигурации nginx хаба — это чужой стек и вне
  репозитория диода, поэтому автоматического гейта на «живую» цепочку нет.
  `message_counter.py` проверяет эхо заглушки и на ptokax-бэкенде **не
  проходит** — его гоняем только на дефолтном стеке.
- `PROXY_RAW_BODY_PASSTHROUGH` оставлен выключенным по умолчанию: заглушка
  говорит на JSON, и включать пропуск мусора на стенде незачем.

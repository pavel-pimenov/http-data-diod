# Round 219 — Стартовая страница-хаб l2-hub со всеми сервисами стека (порт 443)

## Задача
Сделать стартовую страницу на порту 443, на которой располагаются плашки всех
сервисов, доступных в docker-compose, с кратким описанием и ссылками на их
веб-интерфейсы — чтобы проще переключаться между сервисами.

## Реализация
### `www/index.html`
Одностраничный хаб на чистом HTML/CSS/JS (тёмная тема, адаптивная сетка):
- Группы плашек по категориям:
  - **Мониторинг/дашборды**: Grafana :3000, Perses :8089, VictoriaMetrics :8428,
    vmagent :8429, vmalert :8880, Jaeger :16686;
  - **Сервисы l2**: l2-proxy :29002, l2-worker :19091, l2-server :29001,
    l2-db-spider :19094, l2-spider-ai :19096;
  - **NATS**: NATS GUI :7779, NATS Server :8222, nats-exporter :7778;
  - **Ошибки/API**: GlitchTip :8000, Swagger UI :8081, nginx (ODS) :29003;
  - **БД**: Oracle :1521, PostgreSQL :5432.
- Каждая плашка: название (ссылка на веб-интерфейс), краткое описание, порт,
  live-статус (UP/DOWN), проверяемый JS-пингом каждые 10 сек.
- Live-статусы через `Image`-ping (без CORS-блоков на fetch к разным портам).

### Сервис `l2-hub` (docker-compose.yml)
- Статический `nginx:alpine`, отдаёт `./www` на порту **443** (HTTPS).
- Volume `./www:/usr/share/nginx/html:ro`, `./www/nginx/default.conf` → nginx-conf,
  `./certs` → ssl-сертификаты.
- Healthcheck на HTTP `http://localhost/` (порт 80 внутри; busybox wget не умеет TLS).

## Переключение на HTTPS (важно!)
Изначально страница отдавалась на 443 по HTTP, но браузер на `https://nl-sp-p3pg-ad1/`
всегда пробует TLS → не открывалось (SSL_connect fail). Исправление:
- Сгенерирован самоподписанный сертификат `certs/hub.crt`/`hub.key` (CN=nl-sp-p3pg-ad1, 10 лет).
- nginx-конфиг `www/nginx/default.conf`: server 443 ssl (root страница) + server 80
  (только для healthcheck, наружу 80 не проброшен).
- Healthcheck на HTTP 80, т.к. busybox wget в nginx:alpine не поддерживает TLS
  (`--no-check-certificate` нет) и следовал бы за редиректом 80→https и падал на
  верификации сертификата.

## Проверка
- `docker compose config` OK; контейнер Up/**healthy**.
- `curl -sk https://nl-sp-p3pg-ad1/` → HTTP 200, title «l2-http-diod — Хаб сервисов».
- 20 live-статус-индикаторов на странице; ссылки на все веб-интерфейсы на месте.
- В браузере откроется с предупреждением о самоподписанном сертификате — нужно
  подтвердить («перейти на сайт»).

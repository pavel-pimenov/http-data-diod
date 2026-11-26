# round 269 — обновление образов docker-compose до актуальных версий

## Проблема

Образы в `docker-compose.yml` отставали от актуальных релизов на 1–2 года
(Jaeger 2.15.1, VictoriaMetrics 1.97.0, nginx:alpine, Grafana 12.4, GlitchTip 6).
Часть тегов была «плавающей» (`latest`), что делает сборку невоспроизводимой.
Кроме того, версии пакетов дублировались в `src/Dockerfile.test`, README,
TODO и документации.

## Решение

### Образы

В `docker-compose.yml` зафиксированы актуальные на 2026-11-26 версии:

| Сервис | Было | Стало |
|---|---|---|
| nats-server | `nats:2.15.2-alpine` | `nats:2.15.3-alpine` |
| nats-exporter | `0.20.0` | `0.20.3` |
| jaeger | `2.15.1` | `2.23.0` |
| victoria-metrics / vmagent / vmalert | `v1.97.0` | `v1.157.0` |
| l2-test | `curlimages/curl:latest` | `curlimages/curl:8.23.0` |
| glitchtip-db, postgres | `postgres:17-alpine` | `postgres:17.12-alpine` |
| glitchtip | `glitchtip/glitchtip:6` | `glitchtip/glitchtip:7` |
| swagger-ui | `swaggerapi/swagger-ui:latest` | `swaggerapi/swagger-ui:v5.36.0` |
| nginx, l2-hub | `nginx:alpine` | `nginx:1.30.8-alpine` |
| nginx-exporter | `1.1` | `1.5.3` |
| grafana | `grafana/grafana:12.4` | `grafana/grafana:13.3.2` |

PostgreSQL обновлён только патчем внутри мажорной версии 17: volume
`postgres-data` остаётся совместимым, `pg_upgrade` и удаление данных не нужны.
Образы `sphqxe/nats-webui:latest` (новых тегов нет), `gvenzl/oracle-xe:21.3.0-slim`,
`persesdev/perses:v0.55.1` и `ubuntu:26.04` уже актуальны и не тронуты.
Конфиг Jaeger 2.23 проверен: OTLP (4317/4318), Jaeger и Zipkin (9411)
сохранены.

Вне compose синхронизированы ссылки: `src/Dockerfile.test` (NATS 2.15.3 —
бинарник `nats-server` для live-тестов), комментарий в `src/test_nats_live.cpp`,
`README.md` (PostgreSQL, Grafana), `TODO.md` (GlitchTip) и
`docs/http-db-gate-example.md` (PostgreSQL).

### Найденный баг авторизации NATS

Первый после обновления smoke-тест ушёл в 30-секундный таймаут:
`l2-proxy`/`l2-worker` не могли подключиться к NATS (`Authentication Violation`).
Причина — не версия сервера: Compose 5.7.2 склеивал shell-конкатенацию
`'authorization { token: "'"$${NATS_TOKEN}"'" }'` в полностью кавыченную строку,
из-за чего в `/tmp/nats-auth.conf` попадал литерал `token: "${NATS_TOKEN}"`,
а не значение из env. Замена на
`printf 'authorization { token: "%s" }' "$${NATS_TOKEN}"` (и тот же вариант
для user/password) сохраняет интерполяцию и поведение «секрет не в cmdline».
После правки хеш токена в конфиге сервера совпадает с хешем в env клиентов.

## Проверка

- `./rebuild-and-run.sh` — rc=0, все обязательные healthcheck'и зелёные
  (включая Grafana 13, GlitchTip 7 и VictoriaMetrics 1.157);
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0, потерь
  и перекрёстных ответов нет;
- `./health-check.sh all` — rc=0;
- `python3 scripts/metrics-golden-check.py` после трафика — 102/102 семейства;
- `./scripts/ci-gate.sh offline` и `./scripts/ci-gate.sh runtime` — rc=0
  (локально spider поднимается с тестовыми credentials, postgres-профиль не
  включён, поэтому DB-проверки runtime-гейта пропускаются);
- `./scripts/metrics-consistency-check.py --offline` — 122 метрики совпадают
  в C++, Grafana, Perses, README и golden-check.

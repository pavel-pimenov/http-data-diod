# round 211 — GlitchTip для crash-стеков, Grafana анонимная, проверка crash handler

## Задача

Проверить, что обработчик падений (crash handler) работает и стек падения
передаётся в Sentry. Внешний Sentry (`sentry.dp.nlmk.com`) имел rate limit
(429), поэтому перешли на локальный GlitchTip (Sentry-совместимый, всегда часть
compose-стека). Дополнительно сделана Grafana анонимной.

## GlitchTip: создание проекта и DSN

В GlitchTip не было ни пользователей, ни организаций, ни проектов (все таблицы
пустые). Пользователь вручную создал пользователя, организацию и проект.
Получен DSN: `http://caa3f8da3fe34224a86722698bd190b5@.../1`.

### Hostname для DSN внутри docker-сети

Важный нюанс: DSN с `localhost` не работает внутри docker-контейнеров — внутри
контейнера `localhost` это сам контейнер (GlitchTip на хосте недоступен по
`localhost:8000` из контейнера). Hostname `nl-sp-p3pg-ad1` тоже не резолвится
из docker-сети (`Temporary failure in name resolution`). Проверено, что
`host.docker.internal` (extra_hosts: host-gateway, уже настроен в compose)
резолвится в 172.17.0.1 = хост и GlitchTip на нём отвечает.

Итоговый DSN в `.env`:
```
SENTRY_DSN=http://caa3f8da3fe34224a86722698bd190b5@host.docker.internal:8000/1
```
Для внешних клиентов GlitchTip доступен на `http://nl-sp-p3pg-ad1:8000`
(оставлено в комментарии в `.env`).

## Проверка crash handler

Crash handler (src/crash_handler.hpp) установлен на SIGSEGV/SIGABRT/SIGFPE/
SIGBUS/SIGILL. При падении: собирает backtrace, пишет crash dump, форкает child,
который шлёт envelope в Sentry (`send_crash_to_sentry`), родитель ждёт child и
re-raise для core dump.

Запущен crash test (`CRASH_TEST=true`, `MODE=l2-server`) в отдельном контейнере
с тем же образом и SENTRY_DSN на GlitchTip. При старте — raise SIGSEGV.

### Доставка стека в GlitchTip подтверждена

В GlitchTip DB (`issue_events_issueevent`) появилось событие:
- issue: `SIGSEGV: main /app/main.cpp:523` (type=1, level=5=fatal)
- exception.values[0].type = SIGSEGV
- стек символизирован (addr2line + demangle):
  - `CrashHandler::signal_handler` (crash_handler.hpp:475)
  - `main` (main.cpp:523) — место падения
  - `__libc_start_main` (libc.so.6)
  - `_start`

Полный стек (6 фреймов) передан в GlitchTip. Crash handler работает: стек
падения передаётся в Sentry/GlitchTip и символизируется.

## Grafana анонимная

Раньше `GF_AUTH_ANONYMOUS_ENABLED=false` — Grafana требовала логин. Включён
анонимный доступ:
- `GF_AUTH_ANONYMOUS_ENABLED=true`
- `GF_AUTH_ANONYMOUS_ORG_NAME=Main Org.` (единственная org, id=1)
- `GF_AUTH_ANONYMOUS_ORG_ROLE=Viewer`

Проверено: без аутентификации `GET /api/org` → `Main Org.`, доступны 9
дашбордов (L2 Прокси, L2 Воркер, L2 Сервер, NATS, NGINX и др.), страница
`/dashboards` → HTTP 200.

## Проверка доставки транзакций

`python3 scripts/glitchtip-performance-e2e.py --iterations 1 --concurrent 1`:
`PASS: GlitchTip transaction groups grew from 0 to 11 rows` — транзакции
(proxy HTTP INCOMING/POST, worker NATS consume) реально доходят до GlitchTip.

## Файлы

- `.env` — SENTRY_DSN на GlitchTip `host.docker.internal:8000/1` (в gitignore);
- `docker-compose.yml` — Grafana анонимная (GF_AUTH_ANONYMOUS_*).

## Проверки

- `message_counter.py` — Success 1, Failed 0;
- `./health-check.sh all` — все passed (warning о дисковом пространстве 1984 MB).

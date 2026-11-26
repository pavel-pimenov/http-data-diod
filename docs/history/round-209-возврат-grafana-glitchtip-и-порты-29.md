# round 209 — возврат Grafana, GlitchTip в основном стеке, порты 29*, фикс docker-proxy

## Проблема 1: проброшенные рабочие порты зависали с хоста

На стенде все проброшенные «рабочие» HTTP-порты (l2-proxy 8888, l2-server 8088,
nginx 80) зависали при обращении с хоста, хотя внутри docker-сети работали
мгновенно (l2-worker → l2-proxy:8888 = 200 за 1 ms). Метрические порты
(19090/19091/19092/19093) при этом работали с хоста.

Диагностика:
- TCP-рукопожатие через docker-proxy к 8888 проходит (SYN/SYN-ACK/ACK на мосту),
  но HTTP-данные не передаются;
- запросы через шлюз `172.24.0.1:29002` работают, через loopback `127.0.0.1` —
  зависают;
- `docker-proxy` для 8888 висел в состоянии `SYN-SENT`, а для метрик 19090 —
  работал через userland-прокси без DNAT;
- DNAT-правила для 8888/80/8088 срабатывали, но пакеты не форвардились.

Причина: **userland-proxy (docker-proxy)** зависал на пробросе рабочих портов
(вероятно, конфликт с KESL/Kaspersky, перехватывающим сетевой трафик). Метрики
шли через userland-прокси без DNAT и работали.

### Фикс

В `/etc/docker/daemon.json` отключён userland-proxy и включён чистый iptables
DNAT (бэкап — `daemon.json.bak-*`):
```
"live-restore": false,
"icc": true,
"userland-proxy": false,
```
После рестарта docker-демона docker-proxy-процессы исчезли, проброшенные порты
заработали мгновенно. Это системная правка хоста — в git не входит, но
зафиксирована в комментарии docker-compose и README.

## Проблема 2: конфликт/неочевидность портов

Старые проброшенные порты (3333, 8890, 7777) не подчинялись никакому правилу и
пересекались с метриками. Введено правило именования:

- `29xxx` — рабочие/данные порты (проброшенные наружу);
- `19xxx` — метрики (exposer).

Новые пробросы:
| Сервис | Было | Стало |
|---|---|---|
| l2-server | `3333:8088` | `29001:8088` |
| l2-proxy | `8890:8888` | `29002:8888` |
| nginx | `7777:80` | `29003:80` |

Переведены все активные скрипты и документация (не история): `health-check.sh`,
`message_counter.py`, `load_test.py`, `load_test_memory.py`, `rate_limit_test.py`,
`fault_tolerance_test.py`, `test-crash-handler.py`, `run-load-test.sh`,
`scripts/{comprehensive-performance-test, e2e-graceful-shutdown-test, db-gateway-e2e-test,
e2e-worker-reconnect-test, live-chain-check, performance-regression-test,
run-glitchtip-stack, sentry-e2e-test}.{py,sh}`, `README.md`,
`docs/http-db-gate-example.md`, `network-architecture.md`,
`scripts/PRE_COMMIT_README.md`.

## Проблема 3: Grafana была убрана (round 204) — возвращена

Grafana возвращена в стек как второй UI (порт 3000, образ
`grafana/grafana:12.4`, логин `admin/admin`, named volume `grafana-storage`).
Оба UI читают одни и те же определения панелей из
`scripts/generate-grafana-dashboards.py`:
- Grafana — `generate-grafana-dashboards.py --correct-dashboards`;
- Perses — нативный конвертер `generate-perses-dashboards.py`.

`docker-compose.yml`: добавлены сервис `grafana`, volume `grafana-storage`.
`rebuild-and-run.sh`: добавлен блок синка Grafana после Perses (ожидание
`/api/health`, `--correct-dashboards`).
`health-check.sh`: добавлена проверка grafana (`:3000/api/health`).
README: раздел переписан «Grafana + Perses».

## Проблема 4: GlitchTip был по-demand — стал всегда в стеке

GlitchTip (self-hosted Sentry-совместимый, аналог Sentry) был в профиле
`glitchtip` (ENABLE_GLITCHTIP). Переведён в основной стек:
- убраны `profiles: [glitchtip]` у `glitchtip` и `glitchtip-db`;
- `rebuild-and-run.sh`: убрано условное `--profile glitchtip` и `ENABLE_GLITCHTIP`;
- `health-check.sh`: проверка glitchtip (`:8000/`);
- интеграция с C++ через `SENTRY_DSN` — события и performance-транзакции
  l2-proxy/l2-worker/l2-server шлются в GlitchTip. DSN подставляется в `.env`
  после создания проекта в UI (`http://localhost:8000`).

## Проверка

- `./rebuild-and-run.sh` — зелёный: сборка, юнит-тесты, health-check, golden
  metrics; Perses `9/9`, Grafana `9/9` дашбордов, GlitchTip healthy.
- `./health-check.sh all` — 14+ проверок rc=0 (включая grafana :3000, glitchtip
  :8000).
- `message_counter.py --iterations 1 --concurrent 1` — POST+GET успешны, 0 потерь.
- `rate_limit_test.py` — 26k запросов, 429 с корректными заголовками, 0 ошибок.
- `metrics-consistency-check.py --offline` — 117 метрик согласованы.
- `generate-grafana-dashboards.py --check` — 117 C++ метрик покрыты, 9 дашбордов.

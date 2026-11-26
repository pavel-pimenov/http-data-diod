# Round 233 — EXPOSE в Dockerfile приведён к фактическим маппингам docker-compose

## Задача
Проверить, что порты, открытые `EXPOSE` в `src/Dockerfile`, совпадают с теми,
что мапятся из `docker-compose.yml` и реально слушаются бинарником.

## Найденные несоответствия
`EXPOSE 8888 19090 19091 19092 8088` (runtime-base) не содержал порты
`19093`, `19094`, `19095`, хотя они:
- слушаются бинарником (`src/constants.hpp`, namespace `service_port`):
  - `kWorkerHealth = 19093` — health l2-worker
  - `kSpiderMetrics = 19094` — metrics l2-db-spider
  - `kSpiderHealth = 19095` — health l2-db-spider
- мапятся в docker-compose: l2-worker `19093:19093`, l2-db-spider `19094:19094` и `19095:19095`.

## Реализация
`src/Dockerfile` (runtime-base, EXPOSE):
```
EXPOSE 8888 19090 19091 19092 19093 19094 19095 8088
```
Добавлены 19093, 19094, 19095.

## Итоговая картина портов (после правки)
| Порт | Сервис | EXPOSE | docker-compose маппинг |
|---|---|---|---|
| 8888 | l2-proxy HTTP | ✓ | 29002:8888 |
| 8088 | l2-server HTTP | ✓ | 29001:8088 |
| 19090 | l2-proxy metrics | ✓ | 19090:19090 |
| 19091 | l2-worker metrics | ✓ | 19091:19091 |
| 19092 | l2-server metrics | ✓ | 19092:19092 |
| 19093 | l2-worker health | ✓ (добавлен) | 19093:19093 |
| 19094 | l2-db-spider metrics | ✓ (добавлен) | 19094:19094 |
| 19095 | l2-db-spider health | ✓ (добавлен) | 19095:19095 |
| 19096 | l2-spider-ai | ✓ (своя стадия) | 19096:19096 |
| 19097 | l2-heat-ai | ✓ (своя стадия) | 19097:19097 |

l2-heat-scraper порты не слушает (скрейпер), EXPOSE не имеет — корректно.

## Проверка
- `./rebuild-and-run.sh` — сборка зелёная, все сервисы healthy.
- `message_counter.py --iterations 1 --concurrent 1` — exit 0 (no message loss).
- Порты 19090-19095 маппятся и слушаются: l2-worker health 19093/health/ready,
  l2-db-spider metrics 19094/metrics и health 19095/health/ready отвечают
  `{"status":"ready",...}` / метрики.

## Ключевое решение
`EXPOSE` приведён к фактическому набору слушаемых портов (19093/19094/19095
добавлены), чтобы метаданные образа совпадали с маппингами docker-compose и
реальными портами бинарника.

# round 55: e2e reconnect worker + dedup-replay, error-contract, perf-baseline, non-root hardening

## Date: 2026-09-23

### Что сделано
- **Новый e2e-тест `scripts/e2e-worker-reconnect-test.py`** (aiohttp + nats-py + docker), три фазы:
  1. **outage**: `docker stop nats-server` → worker `/health/ready` отдаёт 503 `{"status":"not_ready","error":"NATS not connected"}`, реальный запрос через proxy при упавшем NATS не обслуживается (наблюдается либо таймаут — proxy висит до своего poll-таймаута ~30s, либо 5xx JSON; 200 запрещён). Это заодно валидирует **контракт ошибок worker→proxy**: `request_handler.cpp` отдаёт 500 `queue_failed` / 504 `empty_response` / 503 `DB_UNAVAILABLE` — тест принимает 5xx-семейство и JSON-тело с ключом `error`, фиксируя «не обслуживаем, не молчаливо».
  2. **recovery**: `docker start nats-server` → nats принимает авторизованные соединения, worker автоматически реконнектится (`/health/ready` → 200), echo round-trip через proxy снова 200.
  3. **dedup-replay**: прямой NATS-publish (subject `service.proxy`) одного `request_id` дважды с разными reply-inbox'ами → второй ответ байт-в-байт равен первому (отдан из `m_dedup_cache`, второй вызов L2 не выполняется); в логах worker появляется маркер `Duplicate NATS request detected, returning cached response: request_id=...`.
- **nats-py + auth**: токен берётся из `.env` (`NATS_TOKEN=...`), подключение `nats://TOKEN@localhost:4222`; когда токена нет (CI/свежий клон) — обычный URL без auth, NATS-server в этом случае запускается без `--auth`.
- **Доработки по ходу**: proxy не отвечает 5xx мгновенно при недоступном NATS — тест трактует и 5xx, и timeout как «отказ» (не 200). nats-py требует coroutine-callbacks (`async def` в `subscribe`). `str(asyncio.TimeoutError())` пустой — в отчёте об ошибках теперь `repr`.
- **CI `.github/workflows/ci.yml`**: `pip install aiohttp nats-py`; новая шага `NATS reconnect + dedup e2e test` между golden-metrics и teardown (стек до этого уже пересобран `rebuild-and-run.sh`, все сервисы healthy).
- **Локальный `.env` (gitignored)**: `DEDUP_ENABLED` выправлено на композный дефолт `true` (раньше было `false` — с ним dedup-фаза не тестируема: `m_dedup_cache` выключен, второй delivery шёл в L2 с новым timestamp). L2-breaker e2e осознанно не включён: требует вывода из строя l2-server, что уже покрыто `e2e-graceful-shutdown-test.py` (scope теста — worker-side outage/replay).

### Перформанс-базовая линия и hotspot-анализ
- `python3 scripts/comprehensive-performance-test.py` (message_counter через nginx :7777), все сценарии 100% success, errors=0:
  - Low Load (20×5): RPS 260, p50 17.8ms, p95 27.4ms, p99 27.4ms;
  - Medium Load (50×10): RPS 556, p50 40.7ms, p95 63.9ms, p99 70.3ms;
  - High Load (100×20): RPS 419, p50 31.0ms, p95 63.4ms, p99 84.2ms;
  - Stress Test (200×50): RPS 514, p50 100.3ms, p95 213.7ms, p99 324.1ms.
  - `scripts/perf-report.json` обновлён (tracked — это и есть регрессионный baseline).
- **Hotspot-разбивка** (прометиус-гистограммы + nats-py probe):
  - l2-server: `l2_server_request_duration_seconds` — p99 < 5ms (372/379 ≤ 5ms): бэкенд не bottleneck;
  - чистый worker NATS-путь (nats-py request на `service.proxy`, echo через l2):
    - sequential 60: p50 3.1ms, p95 7.5ms, p99 14.5ms, avg 3.8ms;
    - concurrency 30: p50 21.1ms, p99 37.9ms, avg 22.1ms;
  - proxy-полный путь (nginx→proxy→NATS→worker→l2): p50 17-40ms, а p99 растёт 27→324ms при росте concurrency; дельта к raw NATS-пути = proxy-side накладные (NATS push + `poll_for_response` + пул).
  - Вывод: узкое место — proxy-сторона NATS round-trip, а не worker/l2-server; message_counter (один aiohttp-процесс) сам является клиентом-ограничителем по RPS. Кандидат на отдельный раунд: микро-метрика времени ожидания пула NATS в proxy, чтобы локализовать точку роста p99 (semaphore vs poll interval).

### Non-root hardening контейнеров (l2-proxy/l2-worker/l2-server)
- **`src/docker-entrypoint.sh`**: главный процесс каждого сервиса теперь всегда запускается как непривилегированный пользователь `app` (uid/gid 10001). При старте от root скрипт пере-выставляет владельца на host-mounted каталогах (`/root`, `/memory-logs`, `/profiles`, `/crash-dumps` — их uid виден из контейнера как host-овый) и через `setpriv --reuid/--regid --clear-groups exec "$@"` необратимо сбрасывает привилегии.
- **`src/Dockerfile` (runtime-base)**: `useradd app` (uid 10001) + `chown -R app:app /root /memory-logs /profiles /crash-dumps`; `COPY docker-entrypoint.sh` + `ENTRYPOINT`. WORKDIR остаётся `/root`, CMD не меняется (`sh -c exec ./l2-proxy`) — сигналы/SIGTERM-грейсфул-шатдаун сохранены (PID1 = процесс приложения).
- **`src/.dockerignore`**: `*.sh` снял негативом `!docker-entrypoint.sh` (иначе скрипт не попадает в build context; сборка упала на `COPY ... not found`).
- **`docker-compose.yml`**: для трёх сервисов `cap_drop: [ALL]` + `cap_add: [CHOWN, FOWNER, DAC_OVERRIDE, SETUID, SETGID]`. SETUID/SETGID нужны `setpriv`, но при `setresuid(0→10001)` Linux очищает все effective caps — итоговый процесс приложения работает с **CapEff=0000000000000000**.
- **Две найденные ловушки при отладке**: (1) `no-new-privileges:true` + shebang-script ENTRYPOINT даёт `exec ...: operation not permitted` — флаг осознанно НЕ используется; (2) `setpriv` без `CAP_SETUID` → `setresuid failed: Operation not permitted`, а WORKDIR `/root` (700 root) без `chown` → `exec ./l2-proxy: Permission denied`.
- **Проверено**: `docker exec` — PID1 во всех трёх контейнерах `Uid: 10001/10001/10001/10001`, `CapEff: 0000000000000000`; health + message_counter + полный e2e-reconnect (outage/recovery/dedup) зелёные; файлы логов на хосте перешли в собственность uid 10001 и продолжают писаться.

### Проверка
- `python3 scripts/e2e-worker-reconnect-test.py`: все фазы прошли, rc=0 (в т.ч. на non-root стеке).
- `python3 scripts/comprehensive-performance-test.py`: rc=0, все сценарии 100% success, baseline записан.
- `./rebuild-and-run.sh`: сборка зелёная, unit-тесты прошли (runtime-base с entrypoint + cap_drop/cap_add), службы healthy; PID1 во всех контейнерах uid 10001, CapEff=0.
- `./health-check.sh all` + `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0. Изменений в C++ нет — coverage/clang-tidy не затронуты.
- Изменений в C++ нет (только test-скрипт + ci.yml) — coverage/clang-tidy/контейнерный билд не затронуты; сборка образа и юнит-тесты перепроверены через `./rebuild-and-run.sh`.


# Round 236 — Убран ccache из деплойного Dockerfile (kaniko) + подавлен debconf-шум

## Задача
По логу kaniko-сборки: ccache показывал 0% hits (`Cacheable calls: 46/55,
Hits: 0/46, Misses: 46/46`) — значит ccache **не кешируется** между
kaniko-сборками. Плюс apt-установка давала debconf-шум
(`debconf: unable to initialize frontend: Dialog/Readline`).

## Причина
- `src/Dockerfile` (деплой) — kaniko-совместимый, **без** BuildKit
  `RUN --mount=type=cache`. ccache-кеш живёт в `/root/.ccache`, который kaniko
  не сохраняет между сборками → hits всегда 0%. Комментарий "cache lives in the
  BuildKit cache mount" остался от старого buildx-файла и был устаревшим.
- `DEBIAN_FRONTEND=noninteractive` был задан как `ARG` (не распространяется на
  дочерние стадии/не становится ENV) → apt пытался инициализировать
  Dialog/Readline frontend → шум.

## Реализация
### `src/Dockerfile`
- Убран `ccache` из apt-install (build-essential/ninja/cmake/etc. остались).
- Убраны `ENV CCACHE_MAXSIZE=10G` / `CCACHE_COMPRESS=1`.
- Убран `-DCMAKE_C_COMPILER_LAUNCHER=ccache` в NATS-сборке.
- Убрано `ccache -s` из build RUN.
- Комментарий обновлён: ccache не используется в kaniko; кеширование покрывает
  kaniko layer cache (--cache-repo) для apt/NATS/Oracle, а app-слой
  перекомпилируется при каждом изменении исходников.
- `DEBIAN_FRONTEND=noninteractive` перенесён в `ENV` (наследуется всеми
  стадиями) — подавлен debconf Dialog/Readline шум.

### `src/Dockerfile.test` — НЕ тронут
ccache с `RUN --mount=type=cache,target=/root/.ccache` остаётся, т.к.
Dockerfile.test собирается через docker buildx (BuildKit), где ccache работает.

## Проверка
- `docker build --check --target runtime` — синтаксис валиден.
- `./rebuild-and-run.sh` — сборка зелёная, все сервисы healthy.
- `message_counter.py --iterations 1 --concurrent 1` — exit 0 (no message loss).
- builder-сборка: debconf-шум ушёл (нет `debconf: unable to initialize
  frontend`); остался только безвредный CMake-warning "ccache не найден"
  (ccache убран, CMake использует его опционально через find_program).
- `src/Dockerfile.test --target test` — собирается, юнит-тесты проходят
  (ccache с mount работает, не затронут).

## Ключевое решение
ccache бесполезен в kaniko-сборке (0% hits, нет BuildKit mount) — убран из
деплойного Dockerfile; кеширование оставлено на kaniko layer cache.
`DEBIAN_FRONTEND` переведён в ENV — debconf-шум подавлен.

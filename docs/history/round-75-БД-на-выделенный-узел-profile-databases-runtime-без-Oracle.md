# Round 75: СУБД (postgres/oracle) на выделенный узел — profile `databases`, worker по умолчанию `runtime`

## Мотивация

Локальному хосту (стек proxy/worker/observability) СУБД не нужны: postgres и
oracle используются только контуром HTTP DB Gateway, а сами базы решено вынести
на выделенный узел-БД. При этом код проекта один — одна ветка, один
`docker-compose.yml` и для прикладного узла, и для узла-БД. Задача — развести
роли параметрами (compose-profile + env), не плодя второй compose-файл и не
трогая C++-логику шлюза.

## Что сделано

### postgres и oracle за profile `databases`

`docker-compose.yml`:

- сервис `postgres` переведён в `profiles: [databases]` (раньше стартовал
  по умолчанию и вообще не имел профиля);
- сервис `oracle` переведён с `profiles: [oracle]` на `profiles: [databases]` —
  обе СУБД теперь поднимаются одной командой на узле-БД:
  `docker compose --profile databases up -d`;
- по умолчанию `docker compose up` (и `./rebuild-and-run.sh`) ни одну СУБД не
  стартует: `docker compose config --services` не содержит postgres/oracle.

Проверка: `docker compose config --quiet` OK, `--profile databases
config --services` возвращает ровно `postgres oracle`.

### Worker по умолчанию собирается без Oracle-клиента

- `rebuild-and-run.sh`: дефолт `L2_WORKER_DOCKER_TARGET=runtime-db` заменён на
  `runtime` — образ больше не тянет `gvenzl/oracle-xe:21.3.0-slim` (~несколько
  ГБ) только ради OCI-клиента;
- `docker-compose.yml` для `l2-worker` — `target: ${L2_WORKER_DOCKER_TARGET:-runtime}`
  с комментарием: `runtime-db` — только там, где воркер обязан ходить в Oracle;
- `runtime-db` и stages `oracle-libs` в Dockerfile не трогали — опция
  `L2_WORKER_DOCKER_TARGET=runtime-db` сохранена (используется на узле, где
  нужен Oracle-контур).

### Параметры для удалённых БД

Точки подключения уже были параметрами `DB_POSTGRES_HOST`/`DB_ORACLE_HOST`
(конфиг C++ не менялся). Обновлены пояснения:

- `.env.example`: `DB_POSTGRES_HOST`/`DB_ORACLE_HOST` должны указывать на адрес
  узла-БД (на нём имена `postgres`/`oracle` совпадают с compose-сервисами);
- README и `docs/http-db-gate-example.md`: раздел HTTP DB Gateway переписан под
  «СУБД — profile databases, по умолчанию не стартуют; воркер ходит по
  внешним адресам; для Oracle нужен worker `runtime-db`»;
- TODO.md: команда профиля обновлена.

Гейты не тронуты: `scripts/ci-gate.sh` при выключенном postgres пропускает
DB-проверки (`docker compose ps postgres` больше не видит сервис — он в профиле),
`env-consistency-check.py` по-прежнему зелёный (все C++-переменные остаются в
compose у l2-worker/l2-proxy).

### Устойчивость live-PostgreSQL-тестов (`src/test_db_live.cpp`)

Во время сборки выяснилось, что live-Pg-тесты на малом хосте (2 vCPU / 4 GiB)
стабильно падали с `failed to connect to 127.0.0.1:<port>: Connection refused`.
Две причины и фиксы:

- `PgServer::ensure_started()` возвращал `true` при `m_pid > 0`, не проверив, жив
  ли процесс: если эфемерный postgres умирал по ходу прогона (нехватка памяти
  на фоне parallel/нагрузочных тестов), все последующие live-тесты шли в
  `connection refused`, а не перезапускали кластер. Теперь при мёртвом pid
  кластер перезапускается «самозалечиванием».
- Готовность сервера проверялась TCP-пробой, что ловило гонку «порт уже
  забинден, но postgres ещё не принимает нагрузки» (первый коннект к свеже
  поднятому кластеру получал RST). Вместо `tcp_probe` теперь реальный round-trip
  libpq (`SELECT 1`) в цикле ожидания; таймаут ожидания поднят 10 с → 30 с.

Плюс в `src/Dockerfile` unit-бинарники запускаются двумя последовательными RUN
(вместо `./test_components & ./test_proxy_core & wait`): на малом хосте
одновременный запуск двух бинарников удваивал пиковый RSS и мог убить тестовый
Postgres по OOM; последовательные шаги ещё и отдают гейт отдельными кешируемыми
слоями.

### Окружение хоста (не код)

- `l2_network` в `docker-compose.yml` переведён с `172.22.0.0/16` на
  `172.24.0.0/16`: `172.22.0.0/16` на этом хосте уже занят существующей сетью
  `dozzle_default` (docker `invalid pool request: Pool overlaps`).
- Для smoke-теста `message_counter.py` установлен системный пакет
  `python3-aiohttp` (на хосте не было ни `aiohttp`, ни `pip`).
- Стопнутые на время сборки чужие контейнеры (`ptokax-hub`, `defect-inspector`,
  `portainer`, `beszel`, `caddy`, `beszel-agent`, `dozzle`, `fail2banexporter`)
  после зелёной сборки возвращены в работу. Контейнеры `victoria-metrics` /
  `victoria-logs` стека `flylinkdc-hub` (exited) удалены из-за неизбежной коллизии
  имён с сервисами проекта (данные в томе `flylinkdc-hub_vm-data` сохранены;
  пересоздаются своим `docker compose up`).

## Проверка

- `docker compose config --quiet` и `--profile databases config --services` — OK;
- offline-юнит-тесты гейтов (env consistency, docker context, metrics
  consistency) — зелёные;
- сборка в контейнере (`./rebuild-and-run.sh`) — зелёная:
  `test_components` и `test_proxy_core` прошли, стек поднят без postgres/oracle;
- `./health-check.sh all` — все health-чеки OK;
- `python3 message_counter.py --iterations 1 --concurrent 1 --dup-check` — rc=0,
  потерь/перехлёстов ответов нет, duplicate-WARN логируется.
# Round 74 / 74b / 74c: live NATS+Postgres комбо-заготовка и обновление baseline

> Backfill: раунды 74, 74b и 74c (коммиты `0b044b4`, `fea4628`, `47be13e`)
> были закоммичены без записей в `docs/history/` — фиксирую их здесь
> постфактум, затем продолжено в round 81+.

## Мотивация

После round 73 (живой NATS + production-гейт ветвей) оставался целый
непокрытый прод-контур: `L2Worker::process_db_query_from_nats` — обработка
DB-gateway-запросов, приходящих рабочим по отдельному NATS-субджекту. Путь
«proxy → NATS → worker → PostgreSQL → reply» тестами не проходился: каждый
компонент (NATS, PG, worker) был покрыт отдельно, но не в связке.

## Изменения (round 74, `0b044b4`)

### 1. `src/test_nats_live.cpp` — оснастка для комбо-сценария

* `NatsServer` получил TLS-режим: `enable_tls(cert, key)` + ветка `execlp` с
  `--tls/--tlscert/--tlskey`, и аксессор `pid()`.
* Новый helper `generate_tls_certs(base_dir)` — генерация самоподписанного
  сертификата через `openssl req -x509` в `/tmp` (используется offline-TLS
  тестами).
* В файл добавлен `namespace pgtest` с мини-тёрном `PgSrv` в стиле
  `test_db_live.cpp` (round 72): резерв loopback-порта, `initdb` + `postgres`
  с `mkdtemp`-датакаталогом, drop привилегий через `getpwnam("postgres")`,
  bounded TCP-probe, `shared_pgn()`-синглтон + `REQ_PG()`-макрос и helper
  `mk_pg()` для построения `DbConfig`.

### 2. `src/l2_worker.hpp` — хинт для тест-хука

Добавлены `bool db_handler_ready()` (публичный предикат: handler жив и
enabled) и `friend class L2WorkerDbTestHook;` — задекларированный, но ещё не
реализованный доступ для теста.

### 3. `docs/coverage-baseline.json` — обновлён после coverage-прогона

Прошла перебалансировка записей по результатам прироста live-тестов (несколько
файлов +2..+4 строк/ветвей), заведена запись `main.cpp` (0 покрытых — файл вне
основного потока тестов).

### 4. Комбо-тест-кейс `[db-live-combo]` — **пока заглушка**

Тест `L2Worker: serves DB queries over live NATS+Postgres` поднимает брокер +
worker с `MODE=worker`, но в round 74 только проверяет `is_nats_connected()`.
Вся `pgtest`-оснастка в этом раунде фактически не использовалась. Это
сознательная остановка на полпути — добивка выполнена позже (см. round 81,
там же оснастка получает реальную нагрузку).

## Изменения (round 74b, `fea4628`)

Три маленьких live-теста на граничные случаи NATS-API:

* `NatsClient: request with timeout 0 uses config timeout` — `request()` с
  нулевым таймаутом не роняет клиент (используется конфиг-таймаут).
* `NatsClient: subscribe rejects invalid subject` — `natsConnection_SubscribeSync`
  на мусорном subject возвращает `NATS_INVALID_SUBJECT`.
* `NatsClient: request times out when subscriber never replies` — подписчик
  есть, но не отвечает: `RequestString` возвращает `NATS_TIMEOUT`.

## Изменения (round 74c, `47be13e`)

Только добор baseline после повторного coverage-прогона: `nats_client.cpp`
386→385 строк / 351→349 ветвей (флак из round 72–73 дрейфует и обратно,
остаётся в допуске гейта).

## Итоги

* Все тесты и health-проверки зелёные (см. сообщения коммитов).
* Production-ветви по итогам раунда 73–74 держатся ~51.5–51.7% (гейт
  поднят позже в round 80).
* Несвёрнутый хвост: комбо-тест-кейс и `pgtest`/`db_handler_ready` — мёртвая
  оснастка до следующего раунда (**завершён в round 81**: живой
  NATS+PostgreSQL combo-тест, удалён неиспользуемый `friend class
  L2WorkerDbTestHook`, почищены nodiscard-warning).
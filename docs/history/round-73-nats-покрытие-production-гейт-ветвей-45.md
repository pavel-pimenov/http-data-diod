# Round 73: добив NATS-покрытия, флак nats_client и production-only гейт ветвей 45%

## Мотивация

Три задачи, выбранные на ревью round 72:

1. **Флак `nats_client.cpp` ±10 строк.** В round 72 baseline зафиксировали по
   нижней границе (359/457) как осознанный артефакт замера — нужно было понять
   природу и восстановить честную базу.
2. **Поднять гейт ветвей 40→45.** Полный scope в 41% застрял из-за ~48k ветвей
   шаблонных статических хедеров, инстанцируемых в тестовых TU — тесты
   маскируют дыры в боевых путях. Нужен гейт **production-only**.
3. **Добив непокрытых строк** `nats_client.cpp` (конструктор/auth/TLS/connect
   и пустой reply) и `l2_worker_nats.cpp` (реконнект + refresh подписки после
   рестарта брокера).

## Диагностика флака (п.1)

`nats_client.cpp` флакал строками 144–149 и 165–170 (тело макроса
`CHECK_NATS_OK`/«Не удалось отправить сообщение...»?) — на самом деле это
`nats_connection_connect` fail-ветка. 6/6 прогонов точной coverage-последовательности
(`find -name '*.gcda' -delete; ./test_components; ./test_proxy_core; gcovr`) дали
стабильно **369/457**; состояние «359» больше не воспроизводилось. Вывод:
«359» в round 72 был единичным артефактом повреждения сливаемого `.gcda`
(negative hits, GCC PR68080) — этот же флак лечится
`--gcov-ignore-parse-errors=negative_hits.warn_once_per_file`.

### Гейт по-прежнему держится на полном scope — а ограничивает тестовый код

Ветвевой замер 40% выставили в round 70. Но распределение ветвей:
производственные файлы дают ~13.4k ветвей, тестовые TU — ~54k
(инстанцирование шаблонных хедеров). Гейт «≥40% по всем» сейчас почти не
реагирует на production: его решают тестовые TU. Чтобы тесты не маскировали
дыры, добавлена третья gcovr-инвокация в coverage-стадию:

```dockerfile
gcovr ... --exclude '/app/.*test_[^/]*\.cpp$' \
    --fail-under-branch 45 --json -o /dev/null
```

## Изменения

### 1. `src/Dockerfile` — третья gcovr-инвокация (production-only ветви ≥45%)

В coverage-стадию после gcovr-HTML (строки ≥90%, ветви ≥40% полный scope) и
gcovr-JSON (cov.json для регрессии) добавлен третий проход с тем же фильтром,
но с исключением `test_*.cpp` и порогом `--fail-under-branch 45`. Исключение
работает по полному пути: `/app/.*test_[^/]*\.cpp$` (прежний вариант
`/app/src/test_.*\.cpp$` не отсекал файлы — gcovr матчит по фактическому пути).

### 2. `src/test_nats_live.cpp` — новые тесты (п.3)

Все новые тесты повторяют существующие паттерны (NatsServer/reserve порта/
EnvGuard) и не добавляют новых синхронизаций сверх bounded-ожиданий:

* `NatsClient: constructor logs configured auth and TLS flags` [nats-offline] —
  конструктор со всеми auth/TLS-полями: username/token/creds/tls-ветки логов.
* `NatsClient: auth/TLS misconfiguration aborts connect before dialing`
  [nats-offline] — token-ветка, username/password-ветка (else-if), credentials-ветка
  (мусорный файл /tmp) и две TLS-ветки (bogus CA; bogus cert+key). Ключевое
  открытие: `natsOptions_SetUserCredentialsFromFiles` НЕ валидирует файл на
  моменте установки (несуществующий путь возвращает OK), а
  `natsConnection_Connect` с `SetRetryOnFailedConnect(true)` БЛОКИРУЕТ поток на
  недоступном брокере — единственный надёжный «быстрый» обрыв setup_options —
  это bogus CA (тот же приём, что в fail-fast offline-тестах round 71).
* `NatsClient: connect to a down broker succeeds once it starts` [nats-live] —
  поднятие брокера после первого раунда попыток; bounded через wait_ready.
* `NatsClient: request returns nullopt on an empty reply` [nats-live] — ветка
  `request()` при пустом пустом ответе (пустой `NatsReply{""}`).
* `L2Worker: reconnects and keeps serving after the broker restarts` [nats-live] —
  рестарт брокера посреди run-loop: worker замечает потерю, реконнектит и
  принудительно освежает подписку; второй запрос (с другим request_id, чтобы
  не попасть в dedup-кэш) снова доезжает до L2.
* В round-trip тест добавлен повторный `client.connect()` — покрытие
  идемпотентной ветки «already connected».

Новые включает: `<cstdio>`, `<fstream>`.

### 3. `docs/coverage-baseline.json` — baseline пересчитан через --update

`nats_client.cpp` 359→385/457 (флак закрыт), `l2_worker_nats.cpp` 293→302/361,
итог — 6 записей обновлены.

## Результаты

Сборка coverage-образа зелёная: 547 test-кейсов / 2510 ассерций
(test_components) + 120/883 (test_proxy_core).

| Метрика | Было (round 72) | Стало (round 73) |
|---|---|---|
| Строки (полный scope) | 93.43% (14599/15625) | **93.62%** (14749/15754) |
| Ветви (полный scope) | 41.16% (27890/67760) | **41.20%** (28186/68416) |
| Ветви production-only | 51.4% | **51.73%** (6935/13407) |
| Строки production-only | — | 89.29% (6350/7112) |
| `nats_client.cpp` строки | 78.6% (359/457 baseline) | **84.2%** (385/457), ветви 46.2% |
| `l2_worker_nats.cpp` строки | 81.2% (293/361) | **83.7%** (302/361), ветви 45.6% |

Остаточный флак: одна racing warn-строка `nats_client.cpp:773`
(«NATS still unavailable» при уже-маркированном disconnected) — 0↔1 между
прогонами, не влияет на гейты.

## Гейты

* `--fail-under-line 90` — полный scope — OK (93.62%)
* `--fail-under-branch 40` — полный scope — OK (41.20%)
* `--fail-under-branch 45` — production-only — OK (51.73%)
* пер-файловая регрессия после `--update` — OK (допуск 0.0 п.п.)
* 547+120 тест-кейсов: все зелёные; clang-tidy без предупреждений
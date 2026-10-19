# round 174 — метрики NATS-клиента: таймауты, переподключения, подписки (P1 #4 из TODO)

## Что было не так

У NATS-клиента прокси не было ни одной метрики, кроме агрегатов на стороне
вызывающего (`nats_requests_total`, `nats_errors_total`, `nats_connection_creates_total` ✓,
`nats_connection_errors_total` ✓). Из-за этого по метрикам нельзя было ответить на три
вопроса:

- **таймауты** — сколько NATS-запросов истекло по таймауту (отдельно от «нет
  ответчиков» и от «соединение отвалилось» ✓);
- **переподключения** — как часто NATS-соединение рвётся и восстанавливается ✓ (в
  логах это `NATS reconnected successfully` ✓, но частота «шуршания» NATS по метрикам
  не читалась ✓);
- **подписки** — сколько подписок активно (утечка подписок даёт растущее число
  активных подписок ✓ и незаметную утечку ресурсов ✓).

## Что сделано

- `NatsClient` получил **необязательную** привязку метрик (как circuit breaker
  в раунде 155 ✓ — `set_gauge`/`set_counters` ✓):

  ```cpp
  struct Metrics {
    prometheus::Counter *m_timeouts = nullptr;
    prometheus::Counter *m_reconnects = nullptr;
    prometheus::Gauge *m_subscriptions = nullptr;
  };
  void set_metrics(const Metrics &metrics);
  ```

  Указатели — потому что привязка необязательна: NATS-клиент есть и у прокси ✓,
  и у воркера ✓, а его счётчики живут в другом реестре ✓ (у воркера свои NATS-метрики
  ✓, и в этом TODO они не заводились ✓).

- Три новые метрики в реестре прокси:
  - `l2_proxy_nats_timeouts_total` (counter ✓) — NATS-запрос, истёкший по таймауту
    (статус `NATS_TIMEOUT` ✓; «нет ответчиков» и «соединение отвалилось» считаются
    отдельно ✓, как и раньше);
  - `l2_proxy_nats_reconnects_total` (counter ✓) — успешное переподключение после
    потери соединения ✓;
  - `l2_proxy_nats_subscriptions` (gauge ✓) — число активных подписок ✓
    (обновляется при подписке и при отписке ✓).

- Привязка — в `init_proxy_components()` (`src/proxy_init.cpp` ✓), где создаётся
  NATS-клиент прокси ✓.

- Дашборд L2 Прокси (панель подключений к NATS ✓): линии «Таймауты/с» ✓,
  «Переподключения/с» ✓ и «Подписки» ✓.

- Каталоги README и `CATALOG` golden-check ✓ — итого **100** метрик в четырёх
  источниках ✓.

## Тест

Новый live-тест «NatsClient: the subscription gauge follows
subscribe/unsubscribe» ✓: клиент получает собственный gauge ✓, подписка поднимает
его до 1 ✓, `unsubscribe()` — обнуляет ✓. Счётчики таймаутов и переподключений
проверяются в живом стенде косвенно (таймаут есть в существующем тесте «request
times out when subscriber never replies» ✓, а переподключение — в тестах
переподключения NATS ✓).

## Проверка

- Сборка: `All tests passed (2887 assertions in 601 test cases)` (на один кейс
  больше ✓) и `945 assertions in 125 test cases`.
- `./health-check.sh all` — все проверки пройдены; smoke — все GET-запросы успешны.
- `./scripts/ci-gate.sh runtime` (rc=0): golden `96/96 families … PROXY=33` (было 30 ✓),
  consistency `registered=100 dashboards=100 perses=100 readme=100 golden=100` ✓, parity
  Grafana↔Perses — `OK` ✓.
- `./scripts/run-cppcheck.sh` — rc=0; `./scripts/run-clang-tidy.sh --all` — 47 файлов,
  «no errors or warnings in project files» ✓ (первый прогон указал на недостающий
  `#include <prometheus/registry.h>` в live-тесте ✓ — добавлен ✓).
- Гейт покрытия проверяет CI на этом коммите.

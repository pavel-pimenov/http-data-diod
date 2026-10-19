# round 176 — общий ответчик готовности NATS (прокси и воркер отвечают одинаково)

## Что было не так

`/health/ready` у прокси и у воркера отвечают одинаково, но код ответа был
скопирован в два места:

```cpp
set_nats_readiness_gauges(m_nats_connected, m_health_ready, ready);
if (ready) {
  set_health_ready_nats(res, "l2-proxy");     // или "l2-worker"
} else {
  set_health_not_ready_nats(res, "l2-proxy", error_msg);
}
```

Различались только проверка готовности ✓ (у прокси — реальный health-чек NATS с
текстом ошибки ✓, у воркера — `is_nats_connected()` ✓) и имя сервиса ✓. Сама пара
«зеркалим состояние NATS в gauge'ы + пишем ответ» — одна и та же ✓.

## Что сделано

- В `src/json_response_utils.hpp` рядом с остальными ответами health-check'а появился
  общий помощник:

  ```cpp
  inline void write_nats_readiness(httplib::Response &res,
                                   std::string_view service,
                                   prometheus::Gauge &nats_connected,
                                   prometheus::Gauge &health_ready, bool ready,
                                   std::string_view error);
  ```

  Он делает обе части ✓ (зеркалит gauge'ы ✓ и пишет ответ ✓) — вызывающий код
  отвечает только за *проверку* готовности ✓.

- Обработчики прокси и воркера переведены на него ✓: пять строк в прокси и семь в
  воркере стали одной ✓.

- `set_nats_readiness_gauges()` переехал из `src/metrics_manager.hpp` в
  `src/json_response_utils.hpp` ✓ — после появления помощника он остался бы
  «повисшим» в метриках, а его единственным пользователем стал ответчик
  готовности ✓ (то есть он живёт рядом с `set_health_ready_nats` ✓ и
  `set_health_not_ready_nats` ✓, с которыми он и работает ✓).

Поведение не изменилось ✓: те же HTTP-коды ✓, те же тела ✓, те же gauge'ы ✓.

## Наблюдение (без правки)

Таймаут NATS-запроса сейчас приводит к `mark_disconnected()` ✓ — то есть состояние
соединения помечается как «не подключено» ✓, хотя соединение живо ✓ (истёк только
запрос ✓). Это поведение появилось до текущих раундов ✓ и в этом раунде не
менялось ✓ — но по новой метрике `l2_proxy_nats_timeouts_total` это теперь видно
отдельно ✓ (таймаут запроса ≠ потеря соединения ✓), так что решение о правке можно
принимать с данными ✓.

## Проверка

- Сборка: `All tests passed (2889 assertions in 601 test cases)` и
  `945 assertions in 125 test cases` — набор тестов не изменился ✓ (рефакторинг
  ответа не меняет поведение ✓).
- `./health-check.sh all` — все проверки пройдены; smoke — все GET-запросы успешны.
- `./scripts/ci-gate.sh runtime` (rc=0): golden `96/96 families`, consistency
  `registered=100 dashboards=100 perses=100 readme=100 golden=100` ✓, parity
  Grafana↔Perses — `OK` ✓.
- `./scripts/run-cppcheck.sh` — rc=0; `./scripts/run-clang-tidy.sh --all` — 47 файлов,
  «no errors or warnings in project files» ✓ (первый прогон указал на ставший
  ненужным `#include "metrics_manager.hpp"` в `main.cpp` ✓ — удалён ✓).
- Гейт покрытия проверяет CI на этом коммите.

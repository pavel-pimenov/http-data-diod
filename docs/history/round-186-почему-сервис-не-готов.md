# round 186 — почему сервис «не готов»: `l2_proxy_not_ready_total{reason}` и `l2_worker_not_ready_total{reason}` (P2)

## Что было не так

`/health/ready` у прокси и воркера отвечает **причиной** отказа в теле ответа
(`NATS connection not available` ✓, `NATS ping failed` ✓, `NATS health check
failed: …` ✓), но в **метриках** этой причины не было: `l2_proxy_health_ready` /
`l2_worker_health_ready` — это 0/1 ✓, то есть «не готов» без объяснения ✓.
Вопрос «почему сервис не готов» приходилось решать скрейпом health-эндпоинта или
поиском в логе ✓.

## Что сделано

- Две новые метрики с меткой `reason`:
  - `l2_proxy_not_ready_total{reason}` ✓,
  - `l2_worker_not_ready_total{reason}` ✓.

  Причины — закрытый список из трёх значений ✓ (`nats_unavailable` ✓,
  `ping_failed` ✓, `health_check_failed` ✓), он лежит в `app_context.hpp`
  (`g_not_ready_reasons` ✓) и по нему `AppContext` предсоздаёт серию для каждой
  причины ✓ (иначе семейство не попало бы в `/metrics` до первого отказа ✓).

- Инкременты — в местах, где отказ и возникает ✓:
  - прокси (`RequestHandler::handle_health_ready` ✓): причина определяется там же,
    где формируется текст ошибки ✓ (`ping_failed` ✓ — не прошёл ping ✓,
    `health_check_failed` ✓ — исключение ✓, иначе `nats_unavailable` ✓);
  - воркер (health-сервер в `main.cpp` ✓): единственная причина — `nats_unavailable` ✓.

  Считается **только неготовность** ✓ (не каждый опрос ✓) — то есть это счётчик
  событий «сервис был не готов» ✓, а не счётчик опросов ✓.

- Дашборды: в панели готовности прокси и воркера добавлена линия
  `sum by (reason) (rate(..._not_ready_total[5m]))` ✓.
- Каталоги README и `CATALOG` golden-check ✓ — итого **106** метрик в четырёх
  источниках ✓.

## Замечание о порядке регистрации

Структуры метрик инициализируются **позиционно** ✓, и при добавлении нового
поля порядок регистрации в `init_*_metrics()` обязан совпадать с порядком
объявления ✓ — иначе два однотипных семейства молча меняются местами ✓ (ровно
то, что случилось в раунде 182 ✓). Здесь порядок сразу выровнен ✓, а тест
контракта лейблов (раунд 183 ✓) добавлен в перечень проверяемых семейств ✓ —
поэтому перестановка была бы поймана ✓.

## Проверка

- Python-тесты: `Ran 95 tests … OK` ✓.
- Сборка: `All tests passed (2930 assertions in 606 test cases)` (на шесть утверждений
  больше ✓ — это и есть проверка контракта лейблов двух новых семейств ✓) и
  `945 assertions in 125 test cases` ✓.
- `./health-check.sh all` — все проверки пройдены ✓; smoke — все GET-запросы успешны ✓.
- `./scripts/ci-gate.sh runtime` (rc=0): consistency
  `registered=106 dashboards=106 perses=106 readme=106 golden=106` ✓, parity — `OK` ✓.
- `./scripts/run-cppcheck.sh` — rc=0; `./scripts/run-clang-tidy.sh --all` — 48 файлов,
  «no errors or warnings in project files» ✓.
- Гейт покрытия проверяет CI на этом коммите.

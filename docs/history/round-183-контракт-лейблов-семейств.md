# round 183 — контракт лейблов помеченных семейств + закрытие двух красных прогонов CI

## 1. Контракт лейблов (защита от обмена семейств)

Раунд 182 показал неприятное: структуры метрик инициализируются **позиционно** ✓,
и расхождение порядка объявления полей с порядком регистрации молча меняет местами
два однотипных семейства ✓ (метки приезжают от чужого семейства ✓). Никакой гейт
это не видел ✓: golden-check сверяет **имена** ✓, а имя оставалось правильным ✓.

Легенды дашбордов завязаны на конкретные имена лейблов (`{{type}}` ✓,
`{{reason}}` ✓, `{{outcome}}` ✓, `{{error_class}}` ✓), поэтому перестановка лейблов
тихо ломает панель ✓. Добавлен тест
`AppContext: labelled families keep their label contract` ✓: для каждого помеченного
семейства из контракта проверяется, что у него **ровно** набор лейблов из таблицы ✓
и что набор одинаков во всех режимах ✓.

В контракт входят: `l2_proxy_client_request_errors_total` ✓,
`l2_proxy_rejections_total` ✓, `l2_worker_l2_call_errors_total` ✓,
`l2_worker_retries_total` ✓, `l2_worker_db_query_errors_total` ✓,
`l2_tracing_spans_dropped_total` ✓, `l2_worker_sentry_events_dropped_total` ✓.

Per-database семейства (`l2_proxy_db_requests_total` ✓,
`l2_worker_db_requests_total` ✓) в контракт **не** входят: их серии появляются
только после реального DB-трафика ✓, поэтому в свежем контексте их нет ✓.

## 2. Два красных прогона CI, найденных на раундах 179–182

**`db_query_executor_postgres.cpp` — регрессия покрытия** (−0.70 п.п. ✓): две
новые строки классификатора SQLSTATE не выполнялись ✓ (ветки «нет результата» ✓ и
«код короче двух символов» ✓ — их не покрывал ни один тест ✓). Классификация вынесена
в свободную функцию `db_error_class()` (`src/db_query_utils.hpp` ✓) ✓, а в
исполнителе осталась одна строка ✓; добавлен юнит-тест
`DbErrorClass: the SQLSTATE class is the first two characters` ✓ (обычные коды ✓ и
оба края ✓ — пустой ✓ и слишком короткий ✓ → `unknown` ✓).

**`test_sentry_client.cpp` — гонка в проверке gauge'а очереди**: тест требовал
`m_queue_gauge == 0` сразу после доставки ✓, но поток отправки публикует глубину
очереди **после** извлечения ✓, так что gauge может отстать на мгновение ✓ —
проверка была гонкой ✓ (на CI она и сработала ✓). Заменена на ожидание
`wait_for_condition(..., 2000)` ✓.

## Проверка

- Python-тесты: `Ran 95 tests … OK` ✓.
- Сборка: `All tests passed (2923 assertions in 606 test cases)` (на два кейса и пять
  утверждений больше ✓) и `945 assertions in 125 test cases`.
- `./health-check.sh all` — все проверки пройдены ✓; smoke — все GET-запросы успешны ✓.
- `./scripts/ci-gate.sh runtime` (rc=0): golden `99/99 families`, consistency
  `registered=103 dashboards=103 perses=103 readme=103 golden=103` ✓, parity — `OK` ✓.
- `./scripts/run-cppcheck.sh` — rc=0; `./scripts/run-clang-tidy.sh --all` — 48 файлов,
  «no errors or warnings in project files» ✓.
- Гейт покрытия проверяет CI на этом коммите.

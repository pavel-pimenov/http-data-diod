# round 37: группировка приватных членов DbQueryHandler (State) + RetryHandler (Config/State)

## Date: 2026-09-20

### Что сделано
- `src/db_query_handler.hpp/.cpp`: члены состояния сгруппированы во вложенный struct State (m_executors, m_mutex, m_expected_count, m_configured_names) с экземпляром m_state (в обеих ветках #if flat_map/map). is_enabled/all_configured/init/configured_databases/ready_databases/handle_request переведены на m_state.*; m_pool_metrics остался плоским (единичный, настраивается отдельно).
- `src/retry_handler.hpp`: конфигурация (m_initial_delay_ms, m_max_delay_ms) вынесена в Config, переменное состояние (m_current_delay_ms, m_consecutive_failures) — в State. Конструктор переведён на агрегатную инициализацию m_config + инициализацию m_state; record_failure/record_success работают через m_state/m_config.

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


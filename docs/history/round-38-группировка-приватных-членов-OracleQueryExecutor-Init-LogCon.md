# round 38: группировка приватных членов OracleQueryExecutor (Init) + LogContextScope (Previous)

## Date: 2026-09-20

### Что сделано
- `src/db_query_executor_oracle.hpp/.cpp`: фоновое состояние инициализации ODPI (m_ready, m_stop, m_init_thread) сгруппировано во вложенный struct Init с экземпляром m_init; m_impl остался плоским (одиночный pimpl). Десттруктор/init/is_ready переведены на m_init.*.
- `src/logger.hpp`: LogContextScope — сохранённые значения контекста (m_prev_request_id, m_prev_trace_id, m_prev_client_ip) сгруппированы в Previous (m_previous).

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


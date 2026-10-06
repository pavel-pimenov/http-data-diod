# round 43: группировка приватных членов ThreadPoolWrapper (Backend) + ScopedRequestContext (State)

## Date: 2026-09-20

### Что сделано
- `src/thread_pool_wrapper.hpp`: m_type + m_custom_pool сгруппированы во вложенный struct Backend с экземпляром m_backend (тип пула и его реализация всегда вместе). Конструктор/enqueue/queue_size переведены на m_backend.*.
- `src/common_utils.hpp`: ScopedRequestContext — m_scope + m_client_ip сгруппированы в State (m_state); тело конструктора переведено на m_state.m_client_ip, доступ наружу по-прежнему через client_ip().

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


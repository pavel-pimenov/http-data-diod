# round 48: группировка членов Config (DbQuery)

## Date: 2026-09-22

### Что сделано
- `src/config.hpp`: члены Config HTTP DB Gateway (/v1/sql/{db}/...) сгруппированы во вложенный struct `DbQuery` (m_subject/m_queue_group/m_databases/m_enabled/m_timeout_ms/m_default_timeout_ms/m_default_max_rows) с экземпляром m_db_query. Дефолтные значения и комментарии перенесены в struct-группу.
- Обновлены ссылки в config.cpp (load_db_query_config/validate_nats_and_db_query), request_handler.cpp (роутинг db-gateway), l2_worker_nats.cpp (подписка worker на DB subject), test_components.cpp.

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


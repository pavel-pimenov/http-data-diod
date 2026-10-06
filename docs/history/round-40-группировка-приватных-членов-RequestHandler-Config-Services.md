# round 40: группировка приватных членов RequestHandler (Config/Services)

## Date: 2026-09-20

### Что сделано
- `src/request_handler.hpp/.cpp`: приватные члены RequestHandler сгруппированы во вложенные struct:
  - Config (m_request_timeout_seconds) с экземпляром m_config;
  - Services (m_id_generator, m_push_service, m_poll_service) с экземпляром m_services — бэкенд-стек доставки (ID-генератор + NATS push/poll сервисы).
- m_ctx и m_stats_logger остались плоскими (ссылки на внешние компоненты). Конструктор переведён на агрегатную инициализацию m_config/m_services; обращения в .cpp обновлены.

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


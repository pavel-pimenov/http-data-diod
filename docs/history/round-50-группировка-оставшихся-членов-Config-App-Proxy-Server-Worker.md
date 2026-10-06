# round 50: группировка оставшихся членов Config (App/Proxy/Server/Worker)

## Date: 2026-09-22

### Что сделано
- `src/config.hpp`: оставшиеся члены Config сгруппированы во вложенные struct: `App` (m_mode/m_log_level/m_thread_pool_type/m_crash_test/m_enable_crash_test_endpoint/m_health_ready_allow_connect), `Proxy` (m_protocol/m_port/m_http_pool_size/m_http_pool_idle_timeout_seconds/m_request_timeout_seconds/m_http_timeout_seconds/m_max_retries), `Server` (m_url/m_urls/m_protocol/m_port/m_test_response_delay_ms) и `Worker` (m_threads/m_queue_size) с экземплярами m_app/m_proxy/m_server/m_worker. Убраны секции Group 1/2/5/6 — материал полностью разложен по группам с дефолтами и комментариями.
- Обновлены ссылки: config.cpp (validate/load_*), main.cpp (configure_httplib_server/run/dump), l2_worker.cpp (HttpClientPool/L2Worker ctor), request_handler.cpp, response_builder.cpp, server_handler.cpp, stats_logger.cpp, app_context.cpp, l2_worker_nats.cpp, test_app_context.cpp, test_components.cpp.

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


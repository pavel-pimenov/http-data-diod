# round 33: группировка приватных членов HttpClient (Config/Transport/ConnState) + DedupCache (Config/State)

## Date: 2026-09-20

### Что сделано
- `src/http_client.hpp/.cpp`: приватные члены HttpClient сгруппированы во вложенные struct Config (m_timeout_seconds, m_enable_*, m_ssl_ca_cert_path) / Transport (m_client, m_ssl_client) / ConnState (m_last_status_code, m_is_valid, m_last_used) с экземплярами m_config/m_transport/m_state. Конструктор переведён на агрегатную инициализацию; setup_client/prepare_request/execute_request/getters/touch переведены на сгруппированные имена.
- `src/dedup_cache.hpp`: приватные члены сгруппированы в Config (m_enabled, m_max_entries, m_ttl_ms) / State (m_mutex, m_entries, m_order) с экземплярами m_config/m_state; find/store/evict_* переведены на сгруппированные имена.
- Тесты используют только публичный API — правки тестов не потребовались.

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


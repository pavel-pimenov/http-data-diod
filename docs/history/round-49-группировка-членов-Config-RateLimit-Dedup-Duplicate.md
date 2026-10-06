# round 49: группировка членов Config (RateLimit/Dedup/Duplicate)

## Date: 2026-09-22

### Что сделано
- `src/config.hpp`: члены Config rate limiter/дедупликации/детектора дубликатов сгруппированы во вложенные struct `RateLimit` (вложенные PerIp/Global: m_enabled/m_max_tokens/m_refill_rate/m_max_ips/m_cleanup_ttl_seconds), `Dedup` (m_enabled/m_max_entries/m_ttl_ms) и `Duplicate` (m_enabled/m_reject_enabled/m_top_n/m_max_entries/m_max_body_bytes/m_ttl_ms/m_log_threshold/m_max_clients/m_client_ttl_ms) с экземплярами m_rate_limit/m_dedup/m_duplicate. Дефолтные значения и комментарии перенесены в struct-группы.
- Обновлены ссылки в config.cpp (load тариф-лимитов/дедупликации/дубликатов), proxy_init.cpp (создание PerIPRateLimiter/GlobalRateLimiter/DuplicateDetector), l2_worker.cpp (DedupCache), request_handler.cpp (режект/детект дубликатов), test_components.cpp.

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


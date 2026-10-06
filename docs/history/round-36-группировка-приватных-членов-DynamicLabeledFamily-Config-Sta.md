# round 36: группировка приватных членов DynamicLabeledFamily (Config/State) + CircuitBreaker (Counters)

## Date: 2026-09-20

### Что сделано
- `src/dynamic_labeled_family.hpp`: приватные члены сгруппированы во вложенные struct Config (m_label_name, m_series, m_provider, m_ttl_seconds, m_max_entries, m_histogram_buckets) / State (m_mutex, m_families, m_children, m_last_seen) с экземплярами m_config/m_state. Конструктор переведён на агрегатную инициализацию; get/Collect/ensure_child/remove_label/evict_stale_and_trim/replace_from_provider переведены на сгруппированные имена.
- `src/circuit_breaker.hpp/.cpp`: счётчики сгруппированы в Counters (m_failure_count, m_success_count, m_last_failure_time_us) с экземпляром m_counters; m_gauge и m_state остались плоскими (единичные члены). transition_to_open/allow_request/record_success/record_failure переведены на m_counters.*.
- `src/test_components.cpp`: обращения теста к `cb.m_last_failure_time_us` переведены на `cb.m_counters.m_last_failure_time_us` (+ комментарий).

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


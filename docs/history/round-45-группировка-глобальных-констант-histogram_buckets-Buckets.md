# round 45: группировка глобальных констант histogram_buckets (Buckets)

## Date: 2026-09-20

### Что сделано
- `src/metrics_manager.hpp`: четыре глобальные константы histogram_buckets (g_k_latency_ms_to_5s, g_k_latency_ms_to_10s, g_k_latency_5ms_to_10s, g_k_size_100b_to_5mb) сгруппированы во вложенный struct Buckets (поля m_latency_ms_to_5s, m_latency_ms_to_10s, m_latency_5ms_to_10s, m_size_100b_to_5mb) с единым глобальным экземпляром g_buckets. Наружу доступ через histogram_buckets::g_buckets.m_*.
- Обновлены ссылки в app_context.cpp, proxy_init.cpp, test_coverage_ext.cpp и helper latency_buckets_ms_to_10s().

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


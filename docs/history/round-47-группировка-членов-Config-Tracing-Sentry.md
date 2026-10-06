# round 47: группировка членов Config (Tracing/Sentry)

## Date: 2026-09-22

### Что сделано
- `src/config.hpp`: члены Config, относящиеся к распределённой трассировке и Sentry/GlitchTip, сгруппированы во вложенные struct:
  - `Tracing` (m_enable/m_url/m_sample_rate/m_batch_size/m_flush_interval_ms/m_outage_failure_threshold/m_outage_cooldown_base_ms/m_outage_cooldown_max_ms) с экземпляром m_tracing;
  - `Sentry` (m_dsn/m_environment/m_release/m_sample_rate/m_max_queue_size/m_timeout_ms) с экземпляром m_sentry.
  Дефолтные значения перенесены в struct-группы; удалены пустые комментарии групп полей (Group 3/4), члены которых уехали в группы.
- Обновлены ссылки в config.cpp (load_from_env/validate_tracing), main.cpp (init JaegerLogger/SentryClient), app_context.cpp, test_components.cpp.

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


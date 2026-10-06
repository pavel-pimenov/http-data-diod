# round 35: группировка приватных членов MetricsHistory (Config/Store/Sampler) + DuplicateDetector (State)

## Date: 2026-09-20

### Что сделано
- `src/metrics_history.hpp`: приватные члены сгруппированы во вложенные struct Config (m_registry, m_interval, m_max_series_per_family, m_max_samples) / Store (m_mutex, m_data) / Sampler (m_thread, m_cv_mutex, m_cv) с экземплярами m_config/m_store/m_sampler. Конструктор переведён на агрегатную инициализацию; start/stop/has_family/get_series/series_view/run/sample переведены на сгруппированные имена.
- `src/duplicate_detector.hpp/.cpp`: runtime-состояние сгруппировано в State (m_mutex, m_entries, m_per_client_count) с экземпляром m_state (m_options уже был структурой). record/duplicate_bodies/per_client_count_size/report/evict_* переведены на m_state.*.
- Тесты используют только публичный API — правки тестов не потребовались.

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


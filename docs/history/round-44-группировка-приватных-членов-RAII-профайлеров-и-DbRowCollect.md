# round 44: группировка приватных членов RAII-профайлеров и DbRowCollector (State)

## Date: 2026-09-20

### Что сделано
- Проверка вендорных либ: httplib (0.56.0), nlohmann/json (3.12.0), prometheus-cpp (vendored 1.2.4) — все уже на актуальных upstream-версиях; в деревьях core/pull/util разницы 1.2.4 vs 1.3.0 нет, локальные патчи проекта сохранены. Обновлений не требуется.
- `src/scoped_profiler.hpp`: ScopedProfiler и ScopedLabeledProfiler — члены сгруппированы во вложенный struct State (m_state); конструкторы переведены на designated initializers.
- `src/common_utils.hpp`: RequestScopedTiming — m_profiler/m_counter/m_start_us сгруппированы в State (m_state).
- `src/db_query_utils.hpp`: DbRowCollector — m_max_rows/m_rows/m_truncated сгруппированы в State (m_state).

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.


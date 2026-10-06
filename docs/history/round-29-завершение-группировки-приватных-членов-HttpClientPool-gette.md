# round 29: завершение группировки приватных членов HttpClientPool — getters + cpp (PoolConfig/ConnState/Counters)

## Date: 2026-09-19

### Что сделано
- http_client_pool.hpp: приватный блок сгруппирован во вложенные struct PoolConfig/ConnState/Counters с экземплярами m_config/m_state/m_counters; инлайн-геттеры total_clients()/active_clients() переведены на m_counters.m_*
- http_client_pool.cpp: конструктор переведён на агрегатную инициализацию m_config{...}/m_state{}/m_counters{}; все тела методов переименованы на сгруппированные имена

### Проверка
- Сборка в контейнере ./rebuild-and-run.sh + smoke message_counter.py

## Round 32 (fixed) — 2026-09-20
Сборка приведена к зелёной: исправлен src/json_schema_validator.hpp (добавлен include-guard JSON_SCHEMA_VALIDATOR_HPP, агрегатная инициализация m_limits{...} вместо dotted-ctour-init, get_body_response_ref берётся из json_utils.hpp, без дублирования). Гейт: rebuild+health+smoke rc=0.

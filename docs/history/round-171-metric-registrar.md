# round 171 — `MetricRegistrar`: регистрация метрик без повторения аргумента-реестра

## Что было не так

`src/app_context.cpp` регистрирует **94** метрики, и каждая строка повторяла
аргумент-реестр:

```cpp
MetricsManager::create_counter(
    m_proxy_registry, "l2_proxy_client_requests_total",
    "Total number of client requests received"),
```

Аргумент-реестр — самая длинная часть вызова, и он не меняется от метрики к
метрике: у каждого режима он свой (`m_proxy_registry`, `m_worker_registry`,
`m_server_registry`, `m_common_registry`) и меняется только в пределах блока.
Читается это как шум: имя метрики и её смысл теряются в обвязке вызова.

## Что сделано

- В `src/metrics_manager.hpp` появился `MetricRegistrar` — короткая обёртка над
  реестром:

  ```cpp
  MetricRegistrar const reg_proxy(m_proxy_registry);
  ...
  reg_proxy.counter("l2_proxy_client_requests_total",
                    "Total number of client requests received"),
  ```

  Хелперы те же, что и у `MetricsManager` (`counter`, `gauge`, `histogram`,
  `counter_family`, `gauge_family`, `histogram_family` ✓), все `[[nodiscard]]` ✓
  (clang-tidy требует) и `const` ✓ (регистратор ничего не меняет ✓).

- `app_context.cpp` переведён на короткую форму: 83 вызова переписаны, реестр
  больше не повторяется в каждом ✓. Файл стал компактнее (438 → 432 строки при
  том, что в нём стало на четыре строки больше — сами регистраторы ✓), а
  главное — каждый вызов теперь читается как «имя + смысл» ✓.

Поведение не изменилось: те же имена, те же help-строки, тот же порядок
регистрации ✓ (набор тестов и их количество — те же ✓).

## Что пришлось поправить по ходу (и почему это ценно)

1. **Сканер имён метрик в `scripts/metrics-consistency-check.py` не узнал новую
   форму вызова.** Четыре источника (C++ / дашборды / Perses / README / golden)
   сверяются регуляркой по тексту исходников; после рефакторинга C++-источник
   «содержал» 11 метрик вместо 94 ✗ — и runtime-гейт упал с
   `registered=11 dashboards=94 … FAIL: metric sources disagree` ✓ (это поймал
   локальный прогон гейта ✓). Регулярка расширена на оба вида вызовов:
   `MetricsManager::create_*(registry, "name", …)` и `reg_x.<kind>("name", …)` ✓.

   Это отдельный класс работы: любой рефакторинг, меняющий *текст* регистрации
   метрик, обязан обновить сканер — иначе гейт молча начинает видеть «меньше
   метрик» ✗ (и падает ✓ — хорошо, что падает ✓).

2. **Регистратор для общего реестра сначала создавался до реестра.** Первый
   вариант поставил `MetricRegistrar const reg_common(m_common_registry);` в
   начало `init_common()` — а реестр создаётся несколькими строками ниже ✓ →
   регистратор получил пустой `shared_ptr` → метрики регистрировались «в никуда»
   → падение в юнит-тестах (SIGSEGV на `ResponseBuilder: envelope without
   status_code`) ✓. Локальная сборка это поймала ✓; регистратор перенесён сразу
   после создания реестров ✓.

## Проверка

- Сборка: `All tests passed (2877 assertions in 599 test cases)` и
  `945 assertions in 125 test cases` — набор тестов не изменился ✓.
- `./health-check.sh all` — все проверки пройдены; smoke — все GET-запросы успешны.
- `./scripts/ci-gate.sh runtime` (rc=0): golden `90/90 families`, consistency
  `registered=94 dashboards=94 perses=94 readme=94 golden=94` ✓, parity
  Grafana↔Perses — `OK` ✓.
- `./scripts/run-clang-tidy.sh --all` — 47 файлов, «no errors or warnings in
  project files» ✓; `./scripts/run-cppcheck.sh` — rc=0 ✓.
- `metrics-consistency-check.py --offline` — 94 метрики во всех источниках ✓.
- Гейт покрытия проверяет CI на этом коммите.

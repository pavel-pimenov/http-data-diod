# round 128 — сведение оставшихся env-guard'ов на общий `EnvGuard`

## Что сделано

Round 127 вынес общую RAII-обёртку над env-переменными в
`src/test_support.hpp`, но два тестовых TU держали **свои** одно-переменные
копии того же идиома:

- `test_app_context.cpp` — `AppCtxEnvGuard` (`AppCtxEnvGuard const mode("MODE",
  "worker")`, 2 места);
- `test_components.cpp` — `EnvVarGuard` (78 мест).

Обе копии выставляли одну переменную и **всегда снимали** её в деструкторе
(`unsetenv`), то есть не возвращали прежнее значение (в отличие от `EnvGuard`
из round 127, который его восстанавливает).

Обе копии удалены, оба TU переведены на общий `test_support::EnvGuard`
(`using test_support::EnvGuard;`), а места вызова — на брейс-форму:

```cpp
// было
EnvVarGuard const mode("MODE", "proxy");
// стало
EnvGuard const mode{{"MODE", "proxy"}};
```

Многострочный вызов `L2_SERVER_URLS` с длинным JSON-массивом
(`test_components.cpp`) отформатирован руками, остальные 77 — механически.

Семантика унифицирована по доминирующему в проекте варианту: выход из области
видимости **восстанавливает прежнее значение** переменной (а не безусловно её
снимает). Тесты запускаются в процессе с чистым окружением, поэтому на практике
поведение то же; выигрыш — единый источник идиома и отсутствие копий, которые
могли бы разойтись.

## Проверка

- `./rebuild-and-run.sh` — rc=0, **All tests passed (2816 assertions in 589
  test cases)**.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто. Попутно закрыты два **предсуществующих** (внесённых round
  125) `misc-include-cleaner`-предупреждения в `test_components.cpp`
  (`setup_http_connection`/`setup_ssl_client`): добавлен прямой
  `#include "http_setup.hpp"`.
- coverage (`./scripts/run-coverage.sh`) — rc=0, `production-файлы: 80, строк
  6520/7134 = 91.39%`, `OK: регрессий покрытия нет`. Правки — только тестовые TU,
  поэтому production-покрытие не изменилось (плюс одна строка вернулась из
  тайминг-флейка `metrics_history.hpp`).

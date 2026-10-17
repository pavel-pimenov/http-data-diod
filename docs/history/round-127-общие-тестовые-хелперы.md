# round 127 — общие тестовые хелперы (`test_support.hpp`)

## Что сделано

Три тестовых TU (`test_proxy_handlers.cpp`, `test_l2_worker.cpp`,
`test_nats_live.cpp`) копировали друг у друга один и тот же набор вспомогательных
сущностей; самый крупный пласт — RAII-обёртка над переменными окружения в трёх
дословных копиях (~40 строк каждая), плюс `attach_tracer` (3 копии), `make_get`
(2 копии) и `make_post`.

Все они вынесены в новый **тестовый** заголовок `src/test_support.hpp`
(`namespace test_support`), а из TU удалены:

- `read_env` — чтение переменной окружения в `std::optional<std::string>`;
- `EnvVars` — `std::map<std::string, std::optional<std::string>>`;
- `EnvScope` — общее хранилище/восстановление (`std::vector<pair<имя, прежнее
  значение>>`), с одним `apply(const EnvVars&)`: запоминает прежнее значение и
  выставляет новое (`setenv`/`unsetenv`), а в деструкторе возвращает как было —
  один тест больше не «протекает» в следующий;
- `EnvGuard : EnvScope` — брейс-форма для литеральных оверрайдов
  (`EnvGuard mode{{"MODE", "proxy"}}`, `nullptr` = снять переменную); используется
  `test_proxy_handlers.cpp`;
- `EnvMapGuard : EnvScope` — форма для динамически собранных `EnvVars`
  (`EnvMapGuard env(worker_env(...))`); используется `test_l2_worker.cpp` и
  `test_nats_live.cpp`. Две разные подклассы (а не две перегруженные сигнатуры
  одного класса) выбраны осознанно: `EnvGuard x{{"A","B"}}` и `map`-конструктор
  вместе делают брейс-инициализацию неоднозначной;
- `attach_tracer(AppContext&)` — батчевый Jaeger-экспортёр, нацеленный на
  отказавший порт (спаны буферизуются, flush безвредно падает, трассировочные
  ветви достижимы без бэкенда наблюдаемости);
- `make_get`/`make_post` — сборка `httplib::Request`.

Тела функций/классов не изменены (перенесены дословно, единый источник
поведения). В каждом TU добавлены директивы `using test_support::…;` внутри
анонимного пространства имён, поэтому имена на местах вызова не поменялись, а
обитавшие ранее в анонимных namespace одноклассники с теми же именами убраны.

Заголовок намеренно назван с префиксом `test_`: скрипт
`scripts/coverage-regression-check.py` исключает из гейта и из baseline всё, чьё
имя файла начинается на `test_`, поэтому общий тестовый хелпер не попадает в
production-сравнение (первый глобальный line-гейт в `src/Dockerfile`
считает его наравне со всем кодом, и он полностью исполняется тестами).

## Проверка

- `./rebuild-and-run.sh` — rc=0, **All tests passed (2816 assertions in 589
  test cases)**, `Build successful! Starting containers...`.
- `./health-check.sh all` — rc=0 (proxy/worker/server/nginx/jaeger OK).
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck (`./scripts/run-cppcheck.sh`) — чисто.
- clang-tidy (`./scripts/run-clang-tidy.sh`) — чисто. Первый прогон дал новые
  (внесённые этим раундом) `misc-include-cleaner`-предупреждения: `trace_logger.hpp`
  больше не используется напрямую (его тянет `test_support.hpp`) и
  `test_proxy_handlers.cpp` больше не зовёт `setenv`/`getenv` — лишние прямые
  include удалены, повторный прогон без предупреждений.
- coverage (`./scripts/run-coverage.sh`) — rc=0, `production-файлы: 80, строк
  6519/7134 = 91.38%`, `OK: регрессий покрытия нет`. Новый заголовок имеет
  префикс `test_`, поэтому в per-file baseline не попадает; production-метрика
  не изменилась (правки — тестовые TU + include-only чистка).

Диффстат: `−214 / +45` строк в трёх тестовых TU (+ новый заголовок
`src/test_support.hpp`, 127 строк), чистка лишних include — ещё несколько строк.


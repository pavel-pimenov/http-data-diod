# round 142 — `test_support::{call_get, call_post}`: вызов хендлера одной строкой

## Что сделано

Тесты хендлеров в `src/test_proxy_handlers.cpp` и `src/test_nats_live.cpp`
собирали один и тот же трёхстрочный каркас — собрать запрос, объявить ответ,
позвать хендлер:

```cpp
// было — 41 место
const httplib::Request req = make_get("/health/live");
httplib::Response res;
handler.handle_get(req, res);
```

(для POST — та же тройка с `make_post`/`handle_post`, плюс два места с
`handler.handle_get(make_get("/health/ready"), res);`)

Хелперы в `src/test_support.hpp` — шаблонные, потому что хендлеры в тестах
разных типов (`RequestHandler`, `ServerHandler`, фикстура
`test_support::ProxyHandler`), а нужен от них только публичный
`handle_get`/`handle_post`:

```cpp
template <typename Handler>
[[nodiscard]] inline httplib::Response call_get(Handler &handler,
                                                const std::string &path) {
  const httplib::Request req = make_get(path);
  httplib::Response res;
  handler.handle_get(req, res);
  return res;
}

template <typename Handler>
[[nodiscard]] inline httplib::Response call_post(Handler &handler,
                                                 const std::string &path,
                                                 const std::string &body) {
  const httplib::Request req = make_post(path, body);
  httplib::Response res;
  handler.handle_post(req, res);
  return res;
}
```

Все 43 места стали одной строкой:

```cpp
httplib::Response const res = call_get(handler, "/health/live");
```

Места, где между `make_get` и вызовом хендлера дописываются заголовки
(`req.headers = ...`), не тронуты — там `req` нужен отдельно. `make_get`/
`make_post` остались ровно для таких случаев (`make_get` в
`test_proxy_handlers.cpp` ещё используется, в `test_nats_live.cpp` — больше
нет, поэтому неиспользуемый `using test_support::make_get;` убран).

`using test_support::call_get;`/`call_post;` добавлены в оба TU. Итог: −58
строк в тестах, поведение тестов не менялось (те же проверки на возвращённом
`res`).

## Проверка

- `./rebuild-and-run.sh` — rc=0, все юнит-тесты прошли.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто (он же подтвердил, что шаблон инстанцируется для всех
  типов хендлеров в этих TU).
- coverage (`./scripts/run-coverage.sh`) — rc=0, `OK: регрессий покрытия нет`;
  правка только в `test_`-файле и `test_support.hpp`, production
  6528/7142 = 91.40% без изменений.

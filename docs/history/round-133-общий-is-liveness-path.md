# round 133 — общий `is_liveness_path` для liveness-эндпоинтов

## Что сделано

Условие «это liveness-запрос» было дословно продублировано в двух обработчиках,
отвечающих на `/health` (канонический путь) и `/health/live` (алиас) одинаковым
телом «alive»:

- `RequestHandler::handle_get` (`request_handler.cpp`, l2-proxy);
- `ServerHandler::handle_get` (`server_handler.cpp`, l2-server).

```cpp
// было — дословно в двух файлах
if (req.path == kHealthLivePath || req.path == kHealthPath) {
```

Сам набор путей уже жил одним местом — константами в `url_utils.hpp`; туда же
добавлен предикат:

```cpp
// Liveness endpoints: the canonical path and its /health/live alias are both
// answered with the same "alive" body by the proxy and the server.
[[nodiscard]] inline bool is_liveness_path(std::string_view path) {
  return path == kHealthLivePath || path == kHealthPath;
}
```

Обе точки вызова стали `if (is_liveness_path(req.path))`. Теперь добавление
ещё одного liveness-алиаса — правка одной строки, а не поиск по обработчикам.

Попутно в `url_utils.hpp` добавлен прямой `#include <cstddef>` — файл
использует `size_t` в `parse_url`/`extract_client_ip`/`extract_query_string`
(предсуществующий include-cleaner-gap, всплывший, когда заголовок впервые
попал в набор изменённых для clang-tidy).

## Проверка

- `./rebuild-and-run.sh` — rc=0, **All tests passed (2816 assertions in 589
  test cases)**.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто.
- coverage (`./scripts/run-coverage.sh`) — rc=0, `OK: регрессий покрытия нет`.
  `url_utils.hpp`: 67/67 → 69/69 (новый предикат полностью покрыт тестами обоих
  обработчиков), `request_handler.cpp` 425/446 и `server_handler.cpp` 80/81 — без
  изменений (условие заменено строка-в-строку). Общий production 6532/7146 =
  91.41%, переанкор не потребовался.

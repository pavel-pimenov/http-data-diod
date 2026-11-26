# round 270 — один focused-раунд рефакторинга: чистые хелперы вместо повторяющихся ветвлений

## Проблема

Несколько простых преобразований дублировались по местам вызовов и были
замешаны на сетевом/логирующем коде:

- `handle_http_error()` в `common_utils.cpp` вручную собирал четыре варианта
  сообщения (`url` × `attempt`) и только потом отдавал его в лог;
- `get_header_value()` и `find_header_optional()` в `header_utils.hpp`
  независимо повторяли поиск заголовка с проверкой на пустоту;
- кодирование/декодирование тела по флагу `body_is_binary` повторялось в
  четырёх местах (`request_data_preparer.cpp`, `l2_worker_nats.cpp`,
  `response_builder.cpp`, `worker_request_parser.hpp`) через прямой вызов
  вендорного `<base64.hpp>`.

## Решение

Все три преобразования вынесены в уже существующие заголовки — новые
production-файлы не добавлены:

- `error_types.hpp`: `format_http_error_message(error_msg, operation, attempt, url)`
  — чистый `inline`-хелпер с `[[nodiscard]]` и `std::format`, четыре ветки
  собраны в одном месте. `handle_http_error()` теперь сразу передаёт результат
  в `Logger::error(...)`; счётчики, текст сообщений и порядок логирования не
  изменились.
- `header_utils.hpp`: `find_nonempty_header()` возвращает
  `optional<string_view>` и является единственной точкой поиска;
  `get_header_value()` и `find_header_optional()` — тонкие обёртки. Поведение
  для отсутствующего и пустого заголовка сохранено.
- `base64_utils.hpp`: `encode_if_binary(body, is_binary)` и
  `decode_if_binary(body, is_binary)` инкапсулируют решение «кодировать или
  оставить как есть». Четыре вызова переведены на них, прямой `<base64.hpp>`
  из этих файлов убран; контракт NATS (`kBodyIsBinary`) и лог декодирования
  в `response_builder.cpp` сохранены.

Добавлены юнит-тесты в `src/test_components.cpp`: все четыре формата
`format_http_error_message`, поиск непустого заголовка (есть/нет/пустой) и
условный base64 roundtrip (бинарный и текстовый путь).

## Проверка

- `./rebuild-and-run.sh` — rc=0;
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0;
- `./scripts/run-coverage.sh` — unit-тесты 655 кейсов/3309 assertions
  (`test_components`) + 132/996 (`test_proxy_core`), все прошли; гейты
  `--fail-under-line 90` и `--fail-under-branch 40/50` пройдены, production
  7687/8719 строк = 88.16%;
- Baseline покрытия (`docs/coverage-baseline.json`) обновлён по свежему отчёту:
  семь расхождений в незатронутых файлах (`app_context.cpp`, Oracle,
  `http_client*`, `in_flight_tracker.hpp`) — это накопленный дрейф после merge
  round 266/268, а не потеря покрытия в этом раунде; все изменённые строки
  (`error_types.hpp` 8/8, `base64_utils.hpp` 7/7, хелперы в `header_utils.hpp`
  и `common_utils.cpp`) покрыты. В baseline добавлен `error_types.hpp`;
- `./scripts/run-cppcheck.sh` — без замечаний;
- `./scripts/run-clang-tidy.sh` — только ранее существовавшие предупреждения
  в `header_utils.hpp` и `db_query_utils.hpp`.

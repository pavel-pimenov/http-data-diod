# Раунд 276. JSON-ошибка через `send_json_response()`

Продолжение focused-рефакторинга после round 275. `set_json_error_response()`
в `json_response_utils.hpp` дублировал пару `res.status = ...` +
`res.set_content(..., "application/json")`, уже инкапсулированную в
`send_json_response()`. Теперь хелпер собирает `body` и делегирует запись,
а `send_json_response()` определён выше по файлу (без forward declaration).

## Что сделано

- `src/json_response_utils.hpp`: порядок двух inline-хелперов поменян —
  `send_json_response()` объявлен первым; `set_json_error_response()` вызывает
  его вместо прямого `status`/`set_content`. Статус, тело (`error` и опциональный
  `request_id`) и `Content-Type: application/json` не изменились.
- Вызывающие точки (`response_builder.cpp`, `request_handler_admin.cpp`) и
  NATS-контракт не тронуты.

## Тесты

- `test_components.cpp`: в тесте `set_json_error_response` добавлена явная
  проверка `Content-Type: application/json` — фиксирует, что делегирование
  сохранило заголовок. Остальные assertions на статус и тело без изменений.
- Попутно стабилизирован existing-тест `ThreadPool: active_count follows the
  running tasks`: `std::packaged_task` публикует future до decrement’а
  `m_active_workers`, поэтому сразу после `f.get()` счётчик мог быть ещё равен
  1. Вместо мгновенной проверки — опрос через `wait_for_condition()` до 1 с
  (интервал 1 мс); при реальном зависании воркера тест по-прежнему падает.

## Проверки

- `./scripts/ci-gate.sh offline`, `./scripts/run-cppcheck.sh` — зелёные.
- `./scripts/run-clang-tidy.sh` — только известное не блокирующее предупреждение
  `db_query_utils.hpp:51`.
- `git diff --check` — чисто.
- Первый `./scripts/run-coverage.sh` упал на existing-гонке ThreadPool-теста
  (`active_count()==0` сразу после `f.get()`); после стабилизации повторный
  прогон: все тесты прошли (3332/659 и 999/135), production 88.16%, но
  `coverage-regression-check.py` показал `-0.18 п.п.` по `nats_client.cpp`
  (файл не менялся; вероятен недетерминизм coverage) — вынесено в `TODO.md`.
- `./rebuild-and-run.sh` запущен перед выключением компа, результат не дождан;
  коммит и пуш сделаны по прямой просьбе пользователя без ожидания прогонов.

# Round 105: покрытие query-строки L2-вызова, consume-span в DB-gateway и выбора thread pool

## Что покрыто

1. **`src/l2_worker.cpp`** — `execute_l2_call_with_retry`: добавление query-строки
   к фактическому URL HTTP-вызова (строки 365-366). Тест
   `L2Worker: outbound call routing, ...` теперь дополнительно вызывает
   `call_l2_server("/api/value", "", kTraceparent, {}, "GET", "", "", "filter=1")`
   и проверяет ответ 200 — покрыт non-empty query (ранее всегда пустой).
2. **`src/l2_worker.cpp`** — выбор типа thread pool в ctor: новый
   `TEST_CASE("L2Worker: explicit thread pool type is honoured")` создаёт воркер с
   `THREAD_POOL_TYPE=custom` (ветка CUSTOM, строка 107) и с `MODE=proxy` +
   `THREAD_POOL_TYPE=none` (ветка NONE, не-worker, строка 104). Оба конструируются
   с недоступным брокером и проверяют `!is_nats_connected()`.
3. **`src/request_handler.cpp`** — DB-gateway: при непустом consume-span из NATS
   заголовка `nats_parent_id` переопределяется (строка 875). В live-тесте
   `RequestHandler: DB gateway 200, ...` ответчик теперь возвращает заголовок
   `X-Consume-Span-Id: consume-db-1`, из-за чего покрывается ветка 874-876.

## Результат

| Метрика | До | После |
| --- | --- | --- |
| Утверждения / тест-кейсы | 2683 / 567 | 2687 / 568 |
| Прод-строки (gcovr) | 6442/7146 = 90.15% | 6447/7146 = 90.22% |
| `l2_worker.cpp` строки | 277/313 | 281/313 |
| `request_handler.cpp` строки | 412/449 | 413/449 |

Baseline `docs/coverage-baseline.json` обновлён (78 файлов, регрессий нет).

## Проверка

* `./rebuild-and-run.sh`: rc=0, «All tests passed» (2687 assertions in 568 test
  cases).
* `./health-check.sh all`: все проверки прошли.
* `python3 message_counter.py --iterations 1 --concurrent 1`: success.
* `./scripts/run-cppcheck.sh`: 0 ошибок/предупреждений.
* `./scripts/run-clang-tidy.sh` (изменённые): rc=0, 0 предупреждений.
* `./scripts/run-coverage.sh`: rc=0, гейты пройдены, per-file регрессий нет.

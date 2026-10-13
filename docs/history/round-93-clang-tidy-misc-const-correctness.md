# Round 93: clang-tidy `misc-const-correctness` — 328 `const` в 29 файлах

## Что сделано

Закрыт крупнейший оставшийся автоматизируемый пункт бэклэка round 90 —
`misc-const-correctness`. Добавлено 328 `const` (9 строк с `const` пере-форматированы),
затронуто 29 файлов. `AnalyzePointers: false` (как выбрано в round 89), поэтому
указатели const-ность не трогает — это оставляет правку безопасной и RAII-дружелюбной.

Свип тем же детерминированным способом:

```sh
clang-tidy -p build-lint \
  --checks="-*,misc-const-correctness" \
  --header-filter="^/app/[^/]+\.(hpp|h)$" \
  --format-style=file --fix --fix-errors /app/*.cpp /app/*.cc
```

## Затронутые файлы (добавлено `const`, шт.)

Production: `nats_client.cpp` 12, `trace_logger.cpp` 8, `l2_worker_nats.cpp` 7,
`crash_handler.hpp` 7, `thread_pool.hpp` / `stats_page.hpp` / `sentry_client.cpp` /
`request_handler.cpp` по 3, `server_handler.cpp` / `pool_executor.hpp` / `main.cpp` /
`dedup_cache.hpp` по 2, `l2_worker.cpp` / `dynamic_labeled_family.hpp` /
`crash_utils.hpp` / `stats_logger.cpp` по 1.

Тесты: `test_components.cpp` 128, `test_coverage_ext.cpp` 42,
`test_proxy_core.cpp` 31, `test_trace_logger.cpp` 29, `test_nats_live.cpp` 26,
`test_db_live.cpp` / `test_db_executor_base.cpp` по 3, `test_proxy_handlers.cpp` /
`test_l2_worker.cpp` / `test_app_context.cpp` по 2, `test_sentry_client.cpp` /
`test_http_pipeline.cpp` по 1.

## Найденный баг автофикса (ручная правка)

`clang-tidy` сгенерировал невалидный C++ в `pool_executor.hpp:33`:

```cpp
ReturnType const const result = std::forward<Func>(func)(http_client.get());
```

Двойной `const` — баг фиксера начиная с round 89 (`const` уже вставлен, но фикс
добавил ещё один). Исправлено вручную на `ReturnType const result`. Проверка
`grep 'const const' src` — 0 совпадений. Это первый случай «повреждённого»
автофикса; впредь после каждого `misc-const-correctness`-свипа проверять
`grep -rn 'const const' src`.

## Побочный эффект: файлы стали root-owned

`clang-tidy` запускался в контейнере от root (bind-mount `src`), поэтому все
изменённые файлы получили владельца `root`, и хост-правки (`edit`) падали с
`PermissionDenied`. Восстановлено из root-контейнера:

```sh
docker run --rm -v "$PWD/src:/app" --entrypoint sh http-data-diod:builder \
  -c 'chown 1000:1000 /app/*.cpp /app/*.hpp'
```

На будущее: запускать clang-tidy с `--user $(id -u):$(id -g)`
(+ `-e HOME=/tmp`), чтобы правки сразу принадлежали хост-пользователю.

## Покрытие

`const`-добавления не меняют число instrumentable-строк/ветвей (несколько
строк лишь пере-форматированы `clang-format`). `docs/coverage-baseline.json` не
менялся. Полный coverage-гейт (с 12 live-Postgres тестами) не перепроверялся:
на хосте мало места, а `initdb` ранее падал при заполненном диске.

## Проверка

* `./rebuild-and-run.sh`: rc=0; `=== unit tests (parallel): 93s, all passed ===`.
* `./health-check.sh all`: `All health checks passed!`
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0,
  `Success: All GET requests returned valid binary favicon data`.

Round 91–93 сделаны в одном рабочем дереве (не коммитились раздельно).

## Задел

Остаются: `misc-include-cleaner` по оставшимся файлам, `modernize-avoid-c-arrays`
(64, без автофикса), `modernize-raw-string-literal` (20),
`modernize-use-nodiscard` (14), `readability-identifier-naming` (7),
`modernize-use-ranges` (4).

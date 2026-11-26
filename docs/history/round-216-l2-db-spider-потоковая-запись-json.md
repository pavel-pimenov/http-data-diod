# Round 216 — Потоковая запись JSON в l2-db-spider (инкрементальная сериализация строк)

## Проблема
`l2-db-spider` при опросе БД выполнял запрос через `DbQueryExecutor::execute_query`, который
материализует весь результат в память (json-массив всех строк), затем `data.dump()` сериализует
его целиком. Для ccm4 shared pool это ~113 MB JSON (65 585 строк) — пиковое потребление
доходило до 2 GB (mem_limit 2048m). Требовалось писать JSON на диск инкрементально, не держа
весь результат в памяти.

## Решение
Выбрано: сериализовать каждую строку через nlohmann `json.dump()` (безопасно), а склейку массива
(`[`, `,`, `]`) делать потоково в файле — меньше риска испортить формат JSON, чем рукописная
сериализация.

### Новый интерфейс `DbQueryExecutor::stream_query`
- `src/db_query_executor.hpp`: добавлен чисто-виртуальный `stream_query(sql, params, timeout_ms,
  max_rows, status_code, columns_out, RowCallback row_cb)`, возвращающий число строк.
  `RowCallback = std::function<bool(json&&)>` — вызывается на каждую строку, возвращает false
  для досрочного прекращения. `#include <functional>`.
- Oracle `src/db_query_executor_oracle.{hpp,cpp}`: `stream_query` — копия fetch-цикла
  (acquire/prepare/bind/execute/column descriptor/fetch), но вызывает `row_cb(std::move(row))`
  без накопления в `DbRowCollector`; `max_rows <= 0` = без ограничения; возвращает число строк,
  заполняет `columns_out`.
- Postgres `src/db_query_executor_postgres.{hpp,cpp}`: `stream_query` — копия `execute_query`
  (PQexecParams, bind параметров), но строки отдаются по одной в `row_cb`, без `DbRowCollector`;
  заполняет `columns_out`, возвращает число строк.

### Маршрутизация `DbQueryHandler::stream_request`
- `src/db_query_handler.{hpp,cpp}`: новый публичный метод `stream_request(request, status_code,
  columns_out, row_cb)` — разбирает DbQueryContract-запрос, находит executor, резолвит
  timeout/max_rows и вызывает executor->stream_query. **Фикс**: `status_code = exec_status`
  присваивается из результата executor (без этого статус оставался 0).

### Потоковая запись в `DbSpider`
- `src/db_spider.{hpp,cpp}`: удалён `execute_query`/`write_output`, добавлен `stream_output(db,
  query)`:
  - открывает файл `<output_dir>/<db>_<query>.json`,
  - пишет header `{generated_at, db, query, status:"ok"`,
  - вызывает `m_handler->stream_request` с `row_cb`, который пишет `[`/`,` + `row.dump(-1,' ',
    false, json::error_handler_t::replace)` (replace для невалидного UTF-8 из Oracle sql_text),
  - после строк пишет `],"columns":<columns>,"row_count":<n>}`,
  - при ошибке записывает маленький error-объект `{status:"error", result:{error:{code,message}}}`,
    возвращает HTTP-статус.
  - `poll_database` собирает `{db, driver, timestamp, queries}` с метриками по каждому запросу.

### Тестовые заглушки
- `src/test_db_executor_base.cpp` (`StubExecutor`), `src/test_coverage_ext.cpp` (`ReadyExec`,
  `BasePoolExecMock`): добавлены пустые реализации `stream_query` (иначе классы стали бы
  абстрактными из-за нового чисто-виртуального метода).

## Проверка
- Сборка в контейнере зелёная (gate=0, health=0, smoke=0), все сервисы healthy.
- `message_counter.py --iterations 1 --concurrent 1` — Success (без потерь).
- Живой прогон: `ccm2_shared_pool.json` 17.6 MB, `ccm4_shared_pool.json` 110.8 MB,
  `ccm5_shared_pool.json` 28.2 MB — файлы валидны (закрыты `],\"columns\":...,\"row_count\":65585}`),
  метрики `l2_spider_queries_total{...,status="ok"}` для всех 9 запросов, `polls_total{outcome="ok"}`.
- **Потребление памяти l2-db-spider упало с ~2 GB до ~99 MiB (4.84% от 2GiB)** — потоковая
  запись не материализует результат в памяти.

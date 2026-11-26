# Round 217 — Скрипт анализа spider-output и сборки md-отчёта для AI-агента

## Задача
Написать скрипт на Python, который анализирует JSON-файлы, сгенерированные
`l2-db-spider` в `spider-output/`, и формирует структурированный markdown-отчёт,
который затем передаётся AI-агенту для составления сводного отчёта с
рекомендациями.

## Контекст
В round-216 потоковая запись JSON в l2-db-spider была исправлена: формат файла
теперь валидный JSON:
```json
{"generated_at":..., "db":..., "query":..., "status":"ok",
 "result":{"rows":[[...],...], "columns":[{"name":..,"type":..},...],
           "row_count":N}}
```
или на ошибке:
```json
{"status":"error","result":{"error":{"code":..,"message":..}}}
```

Типы запросов spider:
- `shared_pool` — v$sqlarea (SQL-статистика: executions, cpu_time, elapsed_time,
  buffer_gets, disk_reads, sorts, version_count, invalidations, rows_processed, ...)
- `table_sizes` — размеры сегментов (SEGMENT_NAME, SEGMENT_TYPE, BYTES)
- `tablespaces` — табличные пространства

## Реализация — `scripts/spider_report.py`
- `analyze_file()` — разбор файла на (meta, analysis); корректно обрабатывает
  ok/error-статусы и ошибки парсинга (невалидный JSON).
- Сводная таблица по БД/запросам (статус, строк, время генерации).
- Специфичный анализ по типу запроса через `_ANALYZERS`:
  - `shared_pool`: **максимально расширенный анализ** (по запросу — интересны
    модули-источники вызова):
    - топ SQL по 21 метрике: ELAPSED_TIME, CPU_TIME, EXECUTIONS, BUFFER_GETS,
      DISK_READS, wait-тайминги (APPLICATION_WAIT_TIME, CONCURRENCY_WAIT_TIME,
      CLUSTER_WAIT_TIME, USER_IO_WAIT_TIME), IO-байты (PHYSICAL_READ_BYTES,
      PHYSICAL_WRITE_BYTES, IO_INTERCONNECT_BYTES, IO_CELL_OFFLOAD_ELIGIBLE_BYTES),
      PHYSICAL_READ/WRITE_REQUESTS, ROWS_PROCESSED, SORTS, PARSE_CALLS,
      SHARABLE_MEM, VERSION_COUNT;
    - в каждой топ-таблице колонка **«Источник»**: MODULE / ACTION /
      PROGRAM_ID:PROGRAM_LINE# / PARSING_SCHEMA_NAME — видно, какое
      приложение/модуль генерирует нагрузку (LPC2Enter.exe, NLMKReport.exe,
      JDBC Thin Client, DBMS_SCHEDULER, oracle@host (TNS), zabbix_proxy и т.д.);
    - сводки нагрузки по **MODULE** (источникам), **PARSING_SCHEMA_NAME**
      (схемам), **COMMAND_TYPE** (типам команд) — агрегированные exec/cpu/
      elapsed/buffer/disk/rows;
    - агрегаты пула расширены: + PARSE_CALLS, ROWS_PROCESSED, SHARABLE_MEM
      (в байтах);
    - SQL с высоким VERSION_COUNT (>20) и INVALIDATIONS>0 — с источником.
  - `table_sizes`: топ-20 сегментов по байтам, суммарный размер, разбивка по
    SEGMENT_TYPE.
  - `tablespaces`: таблица NAME/TYPE/SIZE/FREE/USED%.
  - `build_report()` — собирает md-отчёт; для неизвестных запросов показывает
    сырые данные (первые `--top` строк).
  - CLI: `--input-dir` (default `./spider-output`), `--output-md` (иначе stdout),
    `--top` (default 10).
  - Python 3.7-совместимый (без match/case, новых typing).

## Проверка
- `python3 scripts/spider_report.py --output-md /tmp/spider_report.md`:
  отчёт **1555 строк** (было 670); сводка по 12 файлам (9 ok, 3 error — ccm3t
  старые), по 3 живым БД (ccm2/ccm4/ccm5) — по 21 секции SQL-статистики +
  сводки по MODULE/SCHEMA/COMMAND_TYPE + агрегаты + версии/инвалидации.
- Отчёт валиден как markdown, пригоден для передачи AI-агенту; избыточные
  секции при желании отсекаются опциями/фильтрами позже.

## Дополнительно (исправление из round-216)
В процессе тестирования выяснилось, что потоковый формат round-216 был
НЕВАЛИДНЫМ JSON: после `"status":"ok"` массив строк шёл без ключа `"rows"`.
Исправлено в `src/db_spider.cpp::stream_output`: header теперь
`,"status":"ok","result":{"rows":`, footer `],"columns":...,"row_count":N}}`.
После исправления все файлы валидны (`json.load` проходит), `result.rows`
содержит строки. (ccm2 10114, ccm4 64003, ccm5 16234 строк shared_pool.)

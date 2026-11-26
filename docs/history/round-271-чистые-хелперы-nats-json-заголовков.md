# Раунд 271. Чистые хелперы для NATS error-reply, JSON-тела и заголовков

Продолжение focused-рефакторинга из round 270: три маленьких чистых хелпера,
убирающие копирование логики без изменения метрик, логов, контрактов и сетевого
поведения.

## Что сделано

1. **`http_client.hpp::make_nats_error_reply_body(message, detail)`** — единая
   точка сборки тела ошибки NATS-ответа. Раньше `l2_worker_nats.cpp` в двух
   местах вручную собирал JSON: `make_error_json("Invalid request format").dump()`
   для невалидного запроса и `make_error_json("Internal server error")` + запись
   `message = e.what()` в general-catch. Хелпер принимает `std::optional`:
   отсутствие `detail` даёт ровно `{"error":"..."}`, а пустой `detail` даёт
   `{"error":"...","message":""}` — прежняя семантика сохранена (проверка на
   `empty()` была бы ошибкой). Логи `Logger::error`, статусы
   `task.m_activity.m_status` и `record_l2_call_metrics()` остались в вызывающем
   коде.

2. **`json_utils.hpp::is_nonjson_body(body)`** — `!body.empty() && !json::accept(body)`
   переехало из `request_data_preparer.cpp` и `request_handler.cpp` в один хелпер.
   Пустое тело и валидный JSON (включая `null`, число, строку) остаются на
   обычном пути; невалидное/бинарное тело по-прежнему либо base64-кодируется с
   флагом `kBodyIsBinary`, либо отвергается 400-й с `INVALID_JSON` (политика
   `m_raw_body_passthrough` остаётся снаружи хелпера). `json::accept` — только
   синтаксическая проверка, без DOM и без исключений.

3. **Переиспользование `find_nonempty_header()`** (round 270): `extract_client_ip()`
   в `url_utils.hpp` теперь ищет `x-real-ip`, `x-forwarded-for` и
   `cf-connecting-ip` через общий хелпер вместо трёх ручных `find()/!empty()`.
   Приоритет заголовков, выбор последнего XFF-адреса и обрезка `" \t"` не
   изменились. `get_traceparent_header()` в `tracing_helpers.hpp` тоже использует
   хелпер: пустое и отсутствующее значение по-прежнему дают `""`.

## Тесты

- `test_http_pipeline.cpp`: точные bodies для обоих NATS error-reply, пустой
  `detail`, escaping кавычек и перевода строки через `json::parse`.
- `test_components.cpp`: `is_nonjson_body` на пустом теле, валидных JSON-типах,
  мусоре, пробелах и бинарных байтах `\x78\xDA...`.
- `test_proxy_core.cpp`: пустой `x-real-ip` → XFF, пробельный XFF →
  `cf-connecting-ip`, последний XFF-адрес обрезается.
- `test_coverage_ext.cpp`: `traceparent` присутствует, но пуст → `""`.

## Проверки

- `./rebuild-and-run.sh` — зелёный (production-сборка, health, Perses sync).
- `python3 message_counter.py --iterations 1 --concurrent 1` — зелёный.
- `./health-check.sh all` — зелёный.
- `./scripts/ci-gate.sh offline` / `runtime` — зелёные, метрики 122/122,
  golden-check полон, алерты и env-consistency без расхождений.
- `./scripts/run-coverage.sh` — 656 кейсов/3322 assertions (`test_components`)
  и 135/999 (`test_proxy_core`); production 7689/8720 строк = 88.18% (97
  файлов), пер-файловых регрессий нет; два ожидаемых SHIFT (`l2_worker_nats.cpp`,
  `tracing_helpers.hpp`) — код переехал в хелперы, непокрытых строк не прибавилось.
  `docs/coverage-baseline.json` обновлен на новый отчёт.
- `python3 scripts/coverage-regression-check.py` — регрессий покрытия нет.
- `./scripts/run-cppcheck.sh` — без замечаний; clang-tidy — только известные
  предупреждения до этого раунда (новые include-cleanery от правок устранены
  явными include `header_utils.hpp` и `nlohmann/json.hpp`).

Метрики, PromQL, README-каталог и golden-check не менялись: новые хелперы
чистые и не регистрируют метрик.

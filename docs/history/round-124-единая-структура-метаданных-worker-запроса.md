# Round 124: единая структура метаданных worker-запроса

## Что сделано

`L2Worker::RequestData` (`l2_worker.hpp`) дословно повторял 11 полей общего
контракта `worker_request_parser::WorkerRequestData` (`m_request_id`, `m_path`,
`m_query`, `m_method`, `m_body`, `m_client_ip`, `m_proxy_ip`, `m_traceparent`,
`m_proxy_traceparent`, `m_proxy_span_id`, `m_forwarded_headers`) и добавлял
только `TraceContext m_trace_ctx`.

Из-за этого `L2Worker::extract_request_metadata` сразу после
`worker_request_parser::decode_nats_request(...)` перекладывал поля по одному —
11 присваиваний-копий:

```cpp
const worker_request_parser::WorkerRequestData decoded = decode_nats_request(...);
RequestData metadata;
metadata.m_request_id = decoded.m_request_id;
metadata.m_path = decoded.m_path;
... // ещё 9 строк
metadata.m_forwarded_headers = decoded.m_forwarded_headers;
```

Теперь `RequestData` **наследует** общий контракт и лишь добавляет поле,
выводимое worker'ом:

```cpp
struct RequestData : worker_request_parser::WorkerRequestData {
  TraceContext m_trace_ctx;
};
```

а `extract_request_metadata` заполняет базовую подобъектную часть одним
присваиванием (move-семантика `decode_nats_request`) и уже затем выставляет
`m_trace_ctx` из эффективного traceparent:

```cpp
RequestData metadata;
static_cast<worker_request_parser::WorkerRequestData &>(metadata) =
    worker_request_parser::decode_nats_request(request_data);
metadata.m_trace_ctx = handle_trace_context(effective_traceparent, ...);
```

Имена полей совпадают с прежними, поэтому обращения `metadata.m_*` в
`l2_worker.cpp` / `l2_worker_nats.cpp` не менялись; `worker_request_parser`
остаётся чистым контрактом декодера (его не засоряем `TraceContext`).

## Изменения

* **`src/l2_worker.hpp`** — `RequestData` наследует
  `worker_request_parser::WorkerRequestData`, добавляет `m_trace_ctx`; добавлен
  `#include "worker_request_parser.hpp"`.
* **`src/l2_worker.cpp`** — `extract_request_metadata` заполняет базовую часть
  одним присваиванием вместо 11 копий.

Поведение/контракт NATS/вывод не меняются.

## Проверка

* `./rebuild-and-run.sh`: rc=0, юнит-тесты `All tests passed (2816 assertions in 589 test cases)`.
* `./health-check.sh all`: rc=0.
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0.
* cppcheck / clang-tidy по изменённым файлам — новых ошибок нет.
* coverage: `l2_worker.cpp` 313→302 строк (удалены 11 покрытых строк копирования,
  число непокрытых не изменилось — 32) — механический дрейф процента; плюс
  `nats_client.cpp` показал 422→421 покрытую строку (файл не менялся —
  тайминг/порядок-зависимый флейк покрытия). Baseline переанкорен
  (`scripts/coverage-regression-check.py --update`); общий production 91.36%
  (6522/7139).
* `scripts/metrics-consistency-check.py --offline` — ок (метрики не менялись).

# round 152 — именованные константы вместо магических id в тестах трейсинга

## Что сделано

`test_trace_logger.cpp` на 1400 строк повторял одни и те же строковые
литералы: 138 «вхождений» семи значений. Это не просто копипаста — длины здесь
значащие (W3C traceparent требует 32 hex-символа на trace id и 16 на span id),
поэтому опечатка в одном из 37 мест с `"bbbbbbbbbbbbbbbb"` даёт непонятный
провал теста на валидации формата, а не на логике.

Введены семь file-scope констант — в существующем анонимном namespace файла
(там же, где живут `TraceMockServer`/`TraceLoggerEnv`), в стиле уже принятых в
тестах `kTraceparent` (`test_l2_worker.cpp:45`) и `kNatsServerCandidates`
(`test_nats_live.cpp:82`):

```cpp
// Fixed ids used across the tracing tests. Jaeger validates the W3C
// traceparent format, so the lengths matter: 32 hex chars for trace ids and
// 16 for span ids.
constexpr const char *k_test_trace_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char *k_test_trace_id_alt = "0123456789abcdef0123456789abcdef";
constexpr const char *k_test_span_id = "bbbbbbbbbbbbbbbb";
constexpr const char *k_test_span_id_alt = "cccccccccccccccc";
// Nothing listens on port 1, so delivery to this endpoint fails fast: exactly
// what the batch/retry/outage tests need.
constexpr const char *k_dead_jaeger_endpoint = "http://127.0.0.1:1/api/traces";
constexpr const char *k_test_service_name = "test-service";
constexpr const char *k_worker_service_name = "l2-proxy-worker";
```

Замены (138 штук, только самостоятельные литералы):

| константа | было вхождений |
| --- | --- |
| `k_test_span_id` | 37 |
| `k_test_trace_id` | 28 |
| `k_dead_jaeger_endpoint` | 25 |
| `k_test_trace_id_alt` | 14 |
| `k_worker_service_name` | 14 |
| `k_test_service_name` | 13 |
| `k_test_span_id_alt` | 7 |

Каждая константа хранит **ровно ту же строку**, поэтому правка сохраняет
семантику: она не меняет ни одного ожидания в тестах, только убирает копипасту
и даёт значениям имена (проверено сборкой и полным прогоном тестов). В тестах
вида `REQUIRE(span["traceId"] == k_test_trace_id)` теперь сразу видно, что
проверяется именно ожидаемый id, а не «случайная строка из 32 символов».

## Что осознанно не заменено

- **Полные литералы traceparent** (~15 штук вида
  `"00-0123456789abcdef0123456789abcdef-0123456789abcdef-01"`), где id
  встречаются уже *внутри* строки: их бы заменил только хелпер-конструктор
  (`make_traceparent(id, span, sampled)`), а это уже не переименование, а
  изменение способа сборки тестовых данных — отдельная задача. Именно поэтому
  «138» — это про самостоятельные литералы, а не про все вхождения id в файл.
- **Редкие и роле-неоднозначные** 16-hex литералы: `"aaaaaaaaaaaaaaaa"` (8
  вхождений, и в роли короткого trace id, и в роли span id — одно имя скрыло бы
  эту разницу в тестах валидации), `"abcdef0123456789"` (5),
  `"dddddddddddddddd"`/`"1111111111111111"`/`"2222222222222222"` (по 3),
  `"deadbeefdeadbeef"` (2), `"ffffffffffffffff"` (1). Именование здесь добавило
  бы шума без пользы.
- Catch2-теги (`"[tracing]"`, `"[tracing][sentry]"`) — это не данные, а
  разметка тестов.

## Замечено на будущее

`kTraceparent` продублирован в `test_l2_worker.cpp` и `test_nats_live.cpp`
(одинаковая строка в двух файлах) — кандидат в общий тестовый заголовок, если
руки дойдут до такой консолидации.

## Проверка

- clang-tidy `--all` (47 TU) — ноль предупреждений в проектных файлах
  (новые константы проходят `readability-identifier-naming`, как и уже
  существующие `k`-константы в тестах).
- `./rebuild-and-run.sh` — rc=0 (`All tests passed (2814 assertions in
  589 test cases)`).
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck (`./scripts/run-cppcheck.sh`) — чисто.
- coverage (`./scripts/run-coverage.sh`) — rc=0, `coverage unit tests: 106s,
  all passed`, production 6525/7138 = 91.41%, `OK: регрессий покрытия нет`
  (baseline не трогался).

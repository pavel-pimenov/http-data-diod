# round 132 — общий `sentry::service_tags` для тегов Sentry-конвертов

## Что сделано

Оба места, где проект собирает Sentry-полезную нагрузку, открывали блок тегов
одним и тем же кодом (отличались только следующие строки):

- `JaegerLogger::build_sentry_transaction_json` (`trace_logger.cpp`) —
  transaction для performance-отправки;
- `build_event_json` (`sentry_client.cpp`) — error-event.

```cpp
// было — дословно в двух файлах
nlohmann::json tags = nlohmann::json::object();
if (!service_name.empty()) {
  tags["service"] = service_name;
}
```

Общий пролог вынесен в `sentry_client.hpp`/`.cpp` — рядом с остальными
«чистыми» (без сети и реестра) хелперами конверта:

```cpp
// sentry_client.hpp (namespace sentry)
// Sentry tags object seeded with the service name (omitted when empty) — the
// shared opening of the transaction (trace logger) and error-event tag blocks.
nlohmann::json service_tags(const std::string &service_name);
```

```cpp
// sentry_client.cpp
nlohmann::json service_tags(const std::string &service_name) {
  nlohmann::json tags = nlohmann::json::object();
  if (!service_name.empty()) {
    tags["service"] = service_name;
  }
  return tags;
}
```

Точки вызова сведены к одной строке — `sentry::service_tags(service_name)` в
`trace_logger.cpp` и `service_tags(service_name)` в `sentry_client.cpp`
(внутри `namespace sentry`). Остальные теги (`mode`/`request_id`) и
`merge_stringified_tags(...)` остались на местах, вывод JSON не менялся.

Функция объявлена в заголовке, а определена в `.cpp` (как соседние
`build_event_json`/`level_to_string`) — так она тестируема напрямую и не
добавляет строк-артефактов в per-file coverage заголовка.

## Проверка

- `./rebuild-and-run.sh` — rc=0, **All tests passed (2816 assertions in 589
  test cases)**.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто.
- coverage (`./scripts/run-coverage.sh`) — первая проверка дала `REGRESSION
  sentry_client.cpp: 98.25% -> 97.84%` и `REGRESSION trace_logger.cpp: 93.55% ->
  93.51%`. По `trace_logger.cpp` это чистая арифметика: минус 3 покрытые строки
  и плюс 1 (непокрытых как было 24). По `sentry_client.cpp` новый `service_tags`
  добавил единственную непокрытую строку — закрывающую скобку функции (тот же
  gcov-артефакт, что у `generate_event_id` со строки 39, непокрытой и до
  раунда). Baseline переанкорен (`coverage-regression-check.py --update`, заодно
  в него легло улучшение `rate_limiter.hpp` из round 131), повторная проверка —
  `OK: регрессий нет`, production 6530/7144 = 91.41%.

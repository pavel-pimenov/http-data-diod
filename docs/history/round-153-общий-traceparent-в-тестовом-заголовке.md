# round 153 — один traceparent на все тесты (общая константа в `test_support.hpp`)

## Что сделано

Продолжение консолидации тестовых фикстур из round 152: там я отметил, что
`kTraceparent` продублирован в двух тестовых файлах. Это оказалась ровно одна и
та же строка, объявленная дважды в двух анонимных namespace:

```cpp
// test_l2_worker.cpp и test_nats_live.cpp, каждый у себя
constexpr const char *kTraceparent =
    "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
```

Оба файла уже включают `test_support.hpp` — заголовок, который существует
именно для общих тестовых хелперов (появился в round 146/147: app_context,
request/response-стабы, env-гварды, ожидание условия). Константа переехала туда:

```cpp
namespace test_support {

// W3C traceparent carrying a 32-hex trace id: tests that need "the caller
// already started a trace" share this one value instead of repeating it.
constexpr const char *k_traceparent =
    "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
```

Локальные копии удалены, все обращения переписаны на
`test_support::k_traceparent` (5 мест в `test_l2_worker.cpp`, 2 в
`test_nats_live.cpp`). Значение строки не менялось, поэтому поведение тестов
идентично — меняется только место объявления.

Три места вызова в `test_l2_worker.cpp` после подстановки квалифицированного
имени перестали помещаться в 80 колонок (`test_support::k_traceparent` длиннее
локального `kTraceparent`) — переносы переразбиты, новых длинных строк не
добавилось.

## Зачем перенос в заголовок, а не «пусть будет две копии»

Строка трактуется как «вызывающий уже начал трейс»: она попадает в
`NatsContract::kTraceparent` (входящий заголовок) сразу в двух файлах, и если
однажды понадобится другой id или другой флаг sampled (например, чтобы
проверить семплирование на входе), две копии разъедутся молча. Теперь значение
одно, и оно явно помечено как разделяемое.

## Проверка

- clang-tidy `--all` (47 TU) — ноль предупреждений в проектных файлах.
- `./rebuild-and-run.sh` — rc=0 (`All tests passed (2814 assertions in
  589 test cases)`).
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck (`./scripts/run-cppcheck.sh`) — чисто.
- coverage (`./scripts/run-coverage.sh`) — rc=0, `coverage unit tests: 110s,
  all passed`, production 6525/7138 = 91.41%, `OK: регрессий покрытия нет`.

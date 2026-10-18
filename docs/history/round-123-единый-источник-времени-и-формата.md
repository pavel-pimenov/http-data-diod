# Round 123: единый источник времени и RFC3339-формата (`TimeUtils`)

## Что сделано

Найдено упрощённым детектором дублей (максимальные общие блоки токенов) и убрано
ручное дублирование того, что уже есть в `TimeUtils`.

### 1. Ручные `steady_clock`-конверсии в `trace_logger.cpp`

Файл уже пользовался `TimeUtils::steady_ms()` (строка 569), но в трёх местах
повторял тот же пересчёт руками:

* `deliver_sentry_transactions` — `duration_cast<milliseconds>(steady_clock
  ::now().time_since_epoch()).count()` (это ровно `steady_ms()`);
* `enqueue_span` и `send_batch_with_retry` — тот же пересчёт в микросекундах
  (ровно `steady_us()`).

Три блока заменены на `TimeUtils::steady_ms()` / `TimeUtils::steady_us()` —
числовой результат идентичен (те же duration_cast'ы внутри хелперов).

### 2. Дубль RFC3339-формата в `logger.hpp`

`JsonFormatter::format` сам собирал метку времени:

```cpp
const auto now_s = std::chrono::time_point_cast<std::chrono::seconds>(msg.time);
const auto millis = duration_cast<std::chrono::milliseconds>(...).count() % 1000;
j["timestamp"] = std::format("{:%Y-%m-%dT%H:%M:%S}.{:03}Z", now_s, millis);
```

— тот же формат и та же логика `(ms % 1000)`, что и в
`TimeUtils::format_rfc3339_us` (которым уже пользуется трассировщик,
`trace_logger.cpp:236-237`). Теперь JsonFormatter конвертирует `msg.time` в
epoch-микросекунды и вызывает `TimeUtils::format_rfc3339_us(...)`; литерал
формата остался в одном месте. Добавлен `#include "time_utils.hpp"`
(`time_utils.hpp` зависит только от стандартной библиотеки — цикла нет).

Строковый вывод лога не меняется (формат тот же, для неотрицательных времен
`ms % 1000` и `(epoch_us / 1000) % 1000` совпадают).

## Изменения

* **`src/trace_logger.cpp`** — 3× ручной `steady_clock` → `TimeUtils::steady_ms()`
  / `steady_us()`.
* **`src/logger.hpp`** — `JsonFormatter` использует
  `TimeUtils::format_rfc3339_us`, добавлен include `time_utils.hpp`.

## Проверка

* `./rebuild-and-run.sh`: rc=0, юнит-тесты `All tests passed (2816 assertions in 589 test cases)`.
* `./health-check.sh all`: rc=0.
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0.
* cppcheck / clang-tidy по изменённым файлам — новых ошибок нет.
* coverage: удалены только покрытые строки, новых непокрытых **не появилось**
  (число непокрытых строк то же: `logger.hpp` 13, `trace_logger.cpp` 24),
  поэтому пер-файловый процент слегка просел механически
  (`logger.hpp` 276→274 строк, `trace_logger.cpp` 378→372) — baseline
  переанкорен (`scripts/coverage-regression-check.py --update`); общий
  production 91.38% (6534/7150).
* `scripts/metrics-consistency-check.py --offline` — ок (метрики не менялись).

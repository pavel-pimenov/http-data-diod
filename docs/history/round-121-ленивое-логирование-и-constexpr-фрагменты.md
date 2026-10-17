# Round 121: ленивая охрана debug-логов и constexpr-фрагменты чувствительных заголовков

## Что сделано

Две точечные оптимизации горячего пути без изменения поведения.

### 1. `Logger::debug(...)` вычислял аргументы всегда

`Logger::debug(fmt, args...)` — обычный шаблон-функция: spdlog проверяет уровень
уже **после** того, как вызывающий код вычислил все аргументы. Поэтому дорогое
выражение-аргумент исполнялось даже при выключенном DEBUG (уровень по умолчанию
— INFO).

Самое дорогое место — общий обход заголовков `HeaderUtils::filter_headers_impl`
(`header_utils.hpp`): в ветке «форвардим заголовок» вызывался
`redact_header_value(name, value)`, который **возвращает копию значения**
(`std::string`), плюс `to_lower` и скан 12 подстрок-фрагментов. На каждый
форвардимый заголовок (типично 10–20 на запрос) — десятки ненужных аллокаций и
копий значений (cookie/token могут быть крупными), которые сразу выбрасывались.

Добавлен дешёвый pre-check `Logger::should_log(Level)` и им обёрнуто построение
редактированного значения:

```cpp
if (Logger::should_log(Logger::Level::DEBUG)) {
  Logger::debug("{} - Forwarding header: {}: {}", log_context, name,
                redact_header_value(name, value));
}
```

`should_log` реализован как `get_level() <= level` (без нового отображения
уровней и без недостижимых строк — это важно для per-file coverage-гейта).

### 2. `g_sensitive_header_fragments` — динамический `std::vector<std::string>`

Список из 12 подстрок для эвристики «похоже на секрет» хранился как
`std::vector<std::string>`: heap-аллокации и динамическая инициализация на
старте, итерация по `std::string` на каждый заголовок. Заменён на
`inline constexpr std::array<std::string_view, 12>` — ноль аллокаций, ноль
init-стоимости; `get_sensitive_header_fragments()` теперь возвращает
`std::span<const std::string_view>`.

## Изменения

* **`src/logger.hpp`** — новый публичный `should_log(Level)` (pre-check для
  дорогих аргументов логов); `set_level` оставлен как был.
* **`src/header_utils.hpp`**:
  * `g_sensitive_header_fragments`: `std::vector<std::string>` →
    `constexpr std::array<std::string_view, 12>`;
  * `get_sensitive_header_fragments()`: возвращает `std::span<const std::string_view>`;
  * в `filter_headers_impl` построение `redact_header_value(...)` защищено
    `Logger::should_log(Logger::Level::DEBUG)`;
  * удалён ставший неиспользуемым `#include <vector>`.

Публичный API и поведение не менялись: `is_sensitive_header` /
`redact_header_value` / `should_skip_header` работают как прежде (проверены
существующими `[header-utils]`-тестами), тексты логов и их набор сохранены.

## Проверка

* `./rebuild-and-run.sh`: rc=0, юнит-тесты `All tests passed (2816 assertions in 589 test cases)`.
* `./health-check.sh all`: rc=0.
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0 (без потерь/перекрёстных ответов).
* clang-tidy по изменённым файлам: новых ошибок нет; замечания
  `misc-include-cleaner` относятся к предсуществующему шуму при линте
  заголовков отдельно от TU (нет `.cpp` в диффе) и не блокируют гейт.
* cppcheck по изменённым файлам — чисто.
* coverage: `logger.hpp` 95.27%→95.29%, `header_utils.hpp` 99.03%→99.04%,
  общий production 91.40% (6543/7159); регрессий нет, baseline переанкорен
  (`scripts/coverage-regression-check.py --update`) — счётчики строк/ветвей
  обоих заголовков сдвинулись (добавлен `should_log`, охранная ветка,
  фрагменты-`string_view`), плюс ветвь-дрейф `thread_pool_wrapper.hpp`.
* `scripts/metrics-consistency-check.py --offline` — ок (метрики не менялись).

Примечание: на первом прогоне coverage-стадии наблюдался флейк (1 из 589
тестов), повторный прогон идентичной сборки — зелёный; к правке не относится.

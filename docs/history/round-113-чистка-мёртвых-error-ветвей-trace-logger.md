# Round 113: чистка недостижимых error-ветвей `trace_logger` (мёртвые `nullptr`-гварды) и дедупликация breaker-логики

Добиваем «остаток error-ветвей» трассировки: ветви, которые выглядели как
обработка ошибок, но на самом деле недостижимы, и три почти-копии одной и той
же логики breaker-а. Раунд не добавляет тестов — он убирает мёртвый/дублирующий
код, из-за которого в отчёте висели неукрываемые строки и ветви.

## 1. Мёртвые `nullptr`-гварды `acquire_connection()`

`HttpClientPool::acquire_connection()` (`src/http_client_pool.cpp:46-127`) при
исчерпании пула **бросает** `std::runtime_error("HTTP pool acquire timeout …")`,
а не возвращает `nullptr` — ни при таймауте ожидания слота (`:67-79`), ни на
любом другом пути. В `trace_logger.cpp` при этом стояли три «защитные» проверки

```cpp
auto client = pool->acquire_connection();
if (!client) { … return …; }
```

которые не могли сработать ни разу: ветвь `!client` невыполнима, а строка
`if (!client)` в отчёте — вечно непокрытая ветвь. Убраны все три:

* `deliver_sentry_transactions` (Sentry/GlitchTip sink);
* `send_batch` (Jaeger, fire-and-forget);
* `send_span` (Jaeger, одиночный span).

Бро́сок из `acquire_connection()` уже попадает в существующий `catch
(const std::exception &)`, где и учитывается (`spans_failed`), поэтому поведение
не меняется. Рядом оставлен комментарий с контрактом («throws on pool exhaustion
instead of returning a null client»), чтобы гвард не вернули обратно.

**Почему нельзя было просто «покрыть» эти строки:** метод-контракт типа пула
гарантирует non-null, поэтому дойти до `!client` нельзя никаким входом. Это
подтверждает и `src/pool_executor.hpp`: там `nullptr` от `acquire_connection()`
всё ещё возможен — но это шаблон над произвольным типом пула (его
`acquire_connection()` может быть и моком из `test_coverage_ext.cpp:221`),
к `HttpClientPool` отношения не имеет.

## 2. Дедупликация breaker-открытия

Открытие предохранителя (increment `m_consecutive_failures` → проверка порога →
экспоненциальный cooldown → `Logger::warn`) было скопировано **трижды**:
в двух `catch`-ах Sentry-доставки (`std::exception` и `...`) и в `else`-ветви
Jaeger-ретрая. Копии содержали ветвь «порог ещё не достигнут» (`return`), и в
Sentry-копиях она в тестах не исполнялась — то есть один и тот же код был частью
покрыт, частью нет.

Вынесено в приватный `JaegerLogger::record_sink_failure(ExponentialBreaker &,
uint64_t now_steady_ms, std::string_view sink_name)` (`trace_logger.hpp`, объявление
рядом с `deliver_sentry_transactions`). Единственная копия ветви «ниже порога»
теперь достижима: recovery-тест round 112 гоняет `threshold = 1000` и падающий
первый батч, то есть честно проходит через ранний `return`. Семантика
сохранена один-в-один: `(base << min(failures,20)-1)` с клампом на
`cooldown_max_ms`, тот же текст `"{} breaker opened, shedding for {}ms"` и тот же
порядок сообщений (Jaeger сначала «batch send failed, retry», потом «breaker
opened»).

Итог по ветвям: три копии схлопнулись в одну функцию, дублирующиеся
непокрытые ветви исчезли.

## 3. Что осознанно оставлено

* `catch (...)` в `deliver_sentry_transactions` (428-435), `send_batch` (707-712)
  и `send_span` (738-742) — защитные блоки «tracing never crashes»; без
  инъекции неизвестного исключения в `post_no_response`/`acquire_connection` они
  недостижимы, но удалять их нельзя (контракт fire-and-forget).
* `if (batch.empty()) return true;` в `send_batch` — дешёвый defensive guard на
  пустом батче.

## 4. Итог

| метрика | было | стало |
|---|---|---|
| production строки | 6515/7172 = 90.84% | **6505/7147 = 91.02%** |
| production ветви | 7159/13527 = 52.92% | **7149/13505 = 52.94%** |

Файл `trace_logger.cpp`:

| метрика | было | стало |
|---|---|---|
| строки | 394/433 = 90.99% | **384/408 = 94.12%** |
| ветви | 600/1146 = 52.36% | **592/1124 = 52.67%** |

Из знаменателя ушли 25 строк (15 непокрытых + 10 покрытых дублей) и 22 ветви
(14 непокрытых + 8 покрытых дублей) — удалён мёртвый гвард и две копии
breaker-логики. `trace_logger.hpp` без изменений (30/30 строк, 76/167 ветвей).

Baseline переанкорен. Кроме `trace_logger.cpp` в отчёте дрейфанули две
независимые от правки ветви (по −1 покрытой, только branches, гейт построчный):
`stats_page.hpp` 245→244 и `thread_pool.hpp` 84→83 — это инструмент/порядок
тестов, не следствие раунда.

Тесты не менялись: **2755 assertions / 580 cases** (+892/124 во втором бинаре).

## 5. Проверка

* `./scripts/run-coverage.sh` — EXIT=0, оба бинаря «All tests passed», скипов
  нет, гейты (`--fail-under-line 90`, ветви 40 и production 50) пройдены,
  «OK: регрессий покрытия нет».
* `./rebuild-and-run.sh` — EXIT=0, обязательные health-check прошли.
* `./health-check.sh all` — rc=0; `message_counter.py --iterations 1 --concurrent 1`
  — rc=0.
* `./scripts/run-cppcheck.sh` — EXIT=0.
* `./scripts/run-clang-tidy.sh` — EXIT=0.

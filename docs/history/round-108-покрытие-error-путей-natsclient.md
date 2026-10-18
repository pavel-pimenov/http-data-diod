# Round 108: покрытие error-путей NatsClient (пустой subject/header) и мелкие достижимые блоки

Раунд 107 закрылся выводом: у «слабых» production-файлов непокрытые строки —
в основном артефакты атрибуции gcov, а реально достижимых осталось единицы. Этот
раунд — практическая проверка того вывода: отбирали **только те строки, у которых
исполнение окружения реально можно заставить пройти** (т.е. строки внутри целых
неисполненных блоков, а не первые строки многострочных вызовов).

## 1. `nats_client.cpp` — пустой subject и пустой ключ заголовка (+19 строк, +26 ветвей)

Главная находка. Файл — самый слабый по покрытию (406/462 = 87.9%); в нём были
непокрыты **целые блоки** обработки ошибок, которые достижимы детерминированно
через публичный API при живом брокере:

* `request_impl` (`nats_client.cpp:462-466`) — `natsMsg_Create` по пустому
  subject возвращает `NATS_INVALID_ARG` → `set_error` + `return std::nullopt`;
* `request_impl:470` — `set_msg_headers` не смог выставить заголовок;
* `publish:539-544` — `natsConnection_PublishString` по пустому subject
  (`_publishMsg` проверяет `subject == NULL || len == 0`, `pub.c:73-76`) →
  `NATS_INVALID_SUBJECT` → `mark_disconnected` + `return false`;
* `publish_with_headers:718-720` — пустой subject даёт ошибку `natsMsg_Create`;
* `publish_with_headers:724` + `set_msg_headers:689-696` — пустой ключ заголовка:
  `natsMsgHeader_Add` → `_checkMsgAndKey` (`msg.c:185-194`) возвращает
  `NATS_INVALID_ARG` → сообщение уничтожается, `set_error`, `return false`.

Новый live-тест `NatsClient: rejects empty subject and empty header key`
(`src/test_nats_live.cpp`, `[nats-live]`) проверяет все пять путей на одном
подключении к `nats-server`. Ключевая проверка поведения: при отказе
`request`/`publish_with_headers` соединение **не теряется** (`set_error`, но не
`mark_disconnected`) — тест это фиксирует (`REQUIRE(client.is_connected())` между
шагами). Легаси-путь `publish` — единственный, который уводит клиента в
disconnected, поэтому он идёт последним шагом.

Проверено по исходникам вендора, что это именно ошибки библиотеки, а не выдумка
теста: `natsMsg_Create` не валидирует subject (`msg.c`), поэтому пустой subject
не ломает `natsMsg_Create`, а ловится на `_publishMsg`; пустой ключ заголовка
ловится в `_checkMsgAndKey`.

## 2. `in_flight_tracker.hpp` — логи завершения (+2 строки, +4 ветви)

Тесты `[in-flight]` вызывали `wait_for_completion(timeout, false)` — флаг
`log_issues=false` глушил итоговые сообщения, из-за чего неисполненными были
строки 161-162 («Shutdown timeout — ... still in flight») и 168-169 («All
in-flight requests completed»). Оба существующих кейса («Waits for completion»,
«Timeout works») переведены на дефолтный `log_issues=true` — поведение и тайминги
не меняются, исполняются обе ветки лога.

Строки 144 и 168 остаются `count=0` — это «первые строки многострочных
`Logger::{}`» (артефакт, см. round 107 §2а).

## 3. `l2_worker_nats.cpp` — publish без reply-subject (+2 строки)

В существующем live-тесте «L2Worker: run loop serves requests over a live
broker» добавляется fire-and-forget publish на subject воркера (без reply).
Воркер получает сообщение с пустым reply-subject и должен его отбросить
(`l2_worker_nats.cpp:101-102`), а не пытаться ответить в никуда. Детерминизм
обеспечен порядком доставки NATS: следующий по тому же subject запрос
(`not-json` → 400) обрабатывается воркером уже **после** дропа.

## 4. `stats_page.hpp` — «+N more» (+2 ветви, строки не двигаются)

Добавлен тест на плотное семейство (8 серий при `kMaxSeriesPerTile = 6`) —
рендерится строка «+2 more». Ветви +2 (`if (mview.size() > shown)`), но строка
272 остаётся `count=0` — это первая строка многострочного `html << ... << " more"`,
классический артефакт. Тест оставлен как **поведенческий** (фиксирует, что
overflow-строка действительно формируется), а не как покрытийный.

## 5. Что осознанно НЕ бралось (тупики, подтверждённые по коду)

* `l2_worker.cpp:493-494` («Skipping non-POST request») — **мёртвая ветка**:
  `create_standard_request_validator()` (`json_schema_validator.hpp:217-222`)
  уже требует `method ∈ {POST, GET}`, поэтому не-POST не доходит до метода
  `parse_request_data` — валидация падает раньше и отвечает 400;
* `config.cpp:391-394` — `catch` вокруг `JsonUtils::try_parse`, но `try_parse`
  сам ловит `json::parse_error` и возвращает `unexpected` (`json_utils.hpp:91-97`),
  так что исключение в этот `catch` не приходит;
* `http_client.cpp:50` («host is empty») — `parse_url` бросает «Invalid URL»
  раньше, чем управление дойдёт до `setup_client`;
* `stats_page.hpp:255` (`if (family.metric.empty()) return;`) — `Family::Collect`
  не отдаёт семейства без метрик (`family.cc:89-92`), а `Registry::Collect`
  только их и собирает;
* `nats_client.cpp:456/534/710/761` (`acquire_connection` → nullptr),
  `560-561`, `651-665`, `773/783` — достижимы только при гонке между
  `ensure_connected()` и последующей проверкой, либо при неисполнимом
  состоянии; `542-544` ныне покрыт легальным путём пустого subject.

## 6. Итог

* production: **6448/7152 → 6471/7152 = 90.16% → 90.48%** (+23 строки);
* ветви: **7072/13499 → 7108/13499 = 52.39% → 52.66%** (+36);
* per-file: `nats_client.cpp` 406→425 строк (399→425 ветвей),
  `in_flight_tracker.hpp` 55→57 (23→27 ветвей),
  `l2_worker_nats.cpp` 319→321 (395→397 ветвей),
  `stats_page.hpp` ветви 244→246;
* `docs/coverage-baseline.json` переанкорен на новые чтения
  (`coverage-regression-check.py --update`); гейт — «OK: регрессий нет»;
* тест-кейсов +2 (новый live-тест NatsClient + stats-тест «+N more»);
  live-тест воркера и два in-flight расширены без новых кейсов.

## Проверка

* `./scripts/run-coverage.sh` — rc=0; unit-тесты обоих бинарей прошли
  (`All tests passed (2705 assertions in 570 test cases)`,
  `883 assertions in 120 test cases`); гейты строки (≥90%) и ветвей (40%/50%)
  зелёные; per-file гейт — «OK: регрессий покрытия нет (допуск 0.0 п.п.)».
* `./rebuild-and-run.sh` — rc=0, юнит-тесты `2705 assertions in 570 test cases`
  (живые `[nats-live]`-кейсы прошли, без SKIP), все сервисы healthy;
* `python3 message_counter.py --iterations 1 --concurrent 1` — 1/1 успешных,
  0 потерянных/перекрещенных ответов, GET binary-проверка пройдена;
* pre-commit (cppcheck + clang-tidy по изменённым TU) — чисто;
* рабочее дерево — только `src/test_components.cpp`, `src/test_coverage_ext.cpp`,
  `src/test_nats_live.cpp`, `docs/coverage-baseline.json`, `HISTORY.md` и эта
  запись; production-код не менялся.
